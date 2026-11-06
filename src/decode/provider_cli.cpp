#include "provider_cli.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

namespace decode {
namespace {

//   ── 一条"怎么调一个外部程序"的数据 ─────────────────────────────────────────
//   `args` 里的占位符：
//     {in}   = 输入文件绝对路径（包内成员先落临时文件）
//     {page} = 页号（多页文件；单页是 0）
//     {out}  = 期望的输出文件绝对路径（PNG）
//   `info`（可选）= 只读尺寸的命令，stdout 里前两个整数当作 宽 高。
struct CliProfile
{
    QString     name;                 // 稳定 id（进缓存键）
    QString     program;              // 程序名（走 PATH 查）
    QStringList args;                 // 转换命令
    QStringList info;                 // 可选：只读尺寸
    QStringList suffixes;             // **空 = 不广告**（只兜底）
    int         timeoutMs = 30000;
};

//   ★ 内置默认 = **程序名 + 参数模板**，没有一个是格式知识。
//     三个都用"输出到 {out}"的约定；不遵守这个约定的工具靠"临时目录里找第一张 PNG"兜底
//     （`pdftoppm` 之类会写成 `out-1.png`）。
QList<CliProfile> builtinProfiles()
{
    QList<CliProfile> out;

    CliProfile magick;
    magick.name = QStringLiteral("magick");
    magick.program = QStringLiteral("magick");            // ImageMagick 7
    magick.args = {QStringLiteral("{in}[{page}]"), QStringLiteral("-auto-orient"),
                   QStringLiteral("png:{out}")};
    magick.info = {QStringLiteral("-format"), QStringLiteral("%w %h"), QStringLiteral("{in}[{page}]")};
    out << magick;

#if !defined(Q_OS_WIN)
    //   ★★ Windows 上**绝不能**有这条：`convert` 在 Windows 上是
    //     `C:\WINDOWS\system32\convert.exe`（**文件系统 FAT→NTFS 转换工具**），
    //     跟 ImageMagick 6 的 `convert` 撞名 —— 评测时实测就解析到了它。
    //     真去调它可能变成"转换卷"之类的操作，后果不可控 ⇒ 干脆只在非 Windows 注册。
    //     （ImageMagick 7 把命令改名为 `magick` 也正是因为这次撞名。）
    CliProfile convert;                                   // ImageMagick 6
    convert.name = QStringLiteral("convert");
    convert.program = QStringLiteral("convert");
    convert.args = {QStringLiteral("{in}[{page}]"), QStringLiteral("-auto-orient"),
                    QStringLiteral("png:{out}")};
    convert.info = {QStringLiteral("{in}[{page}]"), QStringLiteral("-format"),
                    QStringLiteral("%w %h"), QStringLiteral("info:")};
    out << convert;
#endif

    CliProfile vips;
    vips.name = QStringLiteral("vips");
    vips.program = QStringLiteral("vips");
    vips.args = {QStringLiteral("copy"), QStringLiteral("{in}"), QStringLiteral("{out}")};
    out << vips;

    return out;
}

//   用户的 `decode-tools.json`（`QStandardPaths::AppConfigLocation`）：
//   同名覆盖、新名追加；解析失败只忽略，**绝不影响看图**。
QList<CliProfile> userProfiles()
{
    QList<CliProfile> out;
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                         + QStringLiteral("/decode-tools.json");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return out;
    for (const QJsonValue &v : doc.object().value(QStringLiteral("tools")).toArray()) {
        const QJsonObject o = v.toObject();
        CliProfile p;
        p.name = o.value(QStringLiteral("name")).toString();
        p.program = o.value(QStringLiteral("program")).toString();
        for (const QJsonValue &a : o.value(QStringLiteral("args")).toArray())
            p.args << a.toString();
        for (const QJsonValue &a : o.value(QStringLiteral("info")).toArray())
            p.info << a.toString();
        for (const QJsonValue &a : o.value(QStringLiteral("suffixes")).toArray())
            p.suffixes << a.toString().toLower();
        if (o.contains(QStringLiteral("timeoutMs")))
            p.timeoutMs = o.value(QStringLiteral("timeoutMs")).toInt(p.timeoutMs);
        if (p.name.isEmpty() || p.program.isEmpty() || p.args.isEmpty()) continue;
        out << p;
    }
    return out;
}

QString expand(const QString &arg, const QString &in, const QString &out, int page)
{
    QString s = arg;
    s.replace(QStringLiteral("{in}"), in);
    s.replace(QStringLiteral("{out}"), out);
    s.replace(QStringLiteral("{page}"), QString::number(page));
    return s;
}

class CliProvider final : public Provider
{
public:
    QString name() const override { return QStringLiteral("cli"); }
    int     rank() const override { return 90; }

    //   探活 = 在 PATH 里找到 + **真能跑起来**（`--version` 有输出/不超时）。
    //   ★ 启动期只做这些，**绝不**去枚举工具支持的格式（`magick -list format` 很慢）。
    void probe() override
    {
        m_tools.clear();
        m_suffixes.clear();

        QMap<QString, CliProfile> byName;
        for (const CliProfile &p : builtinProfiles()) byName.insert(p.name, p);
        for (const CliProfile &p : userProfiles()) byName.insert(p.name, p);

        for (const CliProfile &p : byName) {
            const QString exe = QStandardPaths::findExecutable(p.program);
            if (exe.isEmpty()) continue;
            QProcess probe;
            probe.setProgram(exe);
            probe.setArguments({QStringLiteral("--version")});
            probe.start();
            if (!probe.waitForFinished(3000)) {
                probe.kill();
                probe.waitForFinished(500);
                continue;                       // 卡住 ⇒ 当作不可用
            }
            if (probe.exitStatus() != QProcess::NormalExit) continue;
            CliTool t;
            t.profile = p;
            t.exe = exe;
            for (const QString &s : p.suffixes) m_suffixes << s;
            m_tools << t;
        }
        m_suffixes.removeDuplicates();
        m_suffixes.sort();
    }

    bool available() const override { return !m_tools.isEmpty(); }
    QStringList advertisedSuffixes() const override { return m_suffixes; }

    QImage decode(const Source &src, int page, QString *why) override
    {
        if (m_tools.isEmpty()) {
            if (why) *why = QStringLiteral("cli: no tools");
            return {};
        }
        QTemporaryDir tmp;
        if (!tmp.isValid()) return {};

        //   输入：磁盘文件直接用路径；包内成员先落盘（工具都只接受文件）
        QString in;
        if (src.isDisk) {
            in = src.path;
        } else {
            if (src.bytes.isEmpty()) {
                if (why) *why = QStringLiteral("cli: no bytes");
                return {};
            }
            in = tmp.filePath(QStringLiteral("in.bin"));
            QFile f(in);
            if (!f.open(QIODevice::WriteOnly) || f.write(src.bytes) != src.bytes.size()) {
                if (why) *why = QStringLiteral("cli: temp write failed");
                return {};
            }
            f.close();
        }
        const QString out = tmp.filePath(QStringLiteral("out.png"));

        QStringList errors;
        for (const CliTool &t : m_tools) {
            QStringList args;
            for (const QString &a : t.profile.args) args << expand(a, in, out, page);

            QProcess proc;
            proc.setProgram(t.exe);
            proc.setArguments(args);          // ★ 参数数组：**不经 shell**，路径不会被解释
            proc.setWorkingDirectory(tmp.path());
            proc.start();
            if (!proc.waitForFinished(t.profile.timeoutMs)) {
                proc.kill();
                proc.waitForFinished(1000);
                errors << QStringLiteral("%1: timeout").arg(t.profile.name);
                continue;
            }
            if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
                errors << QStringLiteral("%1: rc=%2").arg(t.profile.name).arg(proc.exitCode());
                continue;
            }

            //   输出：优先 `{out}`；工具自己改写文件名（如 `out-1.png`）时找临时目录里的第一张
            QString produced = out;
            if (!QFileInfo::exists(produced)) {
                const QStringList pngs = QDir(tmp.path()).entryList({QStringLiteral("*.png")},
                                                                    QDir::Files, QDir::Name);
                if (pngs.isEmpty()) {
                    errors << QStringLiteral("%1: no output").arg(t.profile.name);
                    continue;
                }
                produced = tmp.filePath(pngs.first());
            }
            QImageReader r(produced);
            r.setAutoTransform(true);
            const QImage img = r.read();
            if (!img.isNull()) return img;
            errors << QStringLiteral("%1: %2").arg(t.profile.name, r.errorString());
        }
        if (why) *why = errors.join(QStringLiteral("; "));
        return {};
    }

    QSize querySize(const Source &src, int page, QString *fmt, int *frames) override
    {
        if (fmt) fmt->clear();
        if (frames) *frames = 1;
        if (!src.isDisk) return {};           // 包内成员：不起子进程量尺寸（太贵）
        QTemporaryDir tmp;
        for (const CliTool &t : m_tools) {
            if (t.profile.info.isEmpty()) continue;
            QStringList args;
            for (const QString &a : t.profile.info)
                args << expand(a, src.path, tmp.filePath(QStringLiteral("out.png")), page);
            QProcess proc;
            proc.setProgram(t.exe);
            proc.setArguments(args);
            proc.start();
            if (!proc.waitForFinished(qMin(t.profile.timeoutMs, 10000))) {
                proc.kill();
                proc.waitForFinished(500);
                continue;
            }
            if (proc.exitCode() != 0) continue;
            const QStringList nums = QString::fromLocal8Bit(proc.readAllStandardOutput())
                                         .split(QRegularExpression(QStringLiteral("[^0-9]+")),
                                                Qt::SkipEmptyParts);
            if (nums.size() >= 2) {
                const int w = nums.at(0).toInt();
                const int h = nums.at(1).toInt();
                if (w > 0 && h > 0) return QSize(w, h);
            }
        }
        return {};
    }

private:
    struct CliTool
    {
        CliProfile profile;
        QString    exe;
    };
    QList<CliTool> m_tools;
    QStringList    m_suffixes;            // 只有用户 profile 显式声明才有内容
};

}  // namespace

Provider *makeCliProvider()
{
    return new CliProvider;               // 探活失败时 `available()` 为 false，链上自动跳过
}

}  // namespace decode

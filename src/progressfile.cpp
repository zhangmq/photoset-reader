#include "progressfile.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace progressfile {
namespace {

constexpr int kMetaReadLimit = 8192;   // 只读元数据时最多读这么多（防御异常文件）
constexpr const char *kMagic = "prsave";
constexpr const char *kSeparator = "---";

// 文本值里的 `\` / 换行 / 回车转义 ⇒ **物理换行永远只来自分行**
QString escaped(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    out.replace(QLatin1Char('\r'), QLatin1String("\\r"));
    return out;
}

QString unescaped(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\') && i + 1 < s.size()) {
            const QChar n = s.at(++i);
            if (n == QLatin1Char('n'))       out += QLatin1Char('\n');
            else if (n == QLatin1Char('r'))  out += QLatin1Char('\r');
            else if (n == QLatin1Char('\\')) out += QLatin1Char('\\');
            else { out += c; out += n; }
        } else {
            out += c;
        }
    }
    return out;
}

QString kindName(Sequence::Kind k)
{
    switch (k) {
    case Sequence::Kind::Folder:  return QStringLiteral("folder");
    case Sequence::Kind::Archive: return QStringLiteral("archive");
    case Sequence::Kind::Image:   return QStringLiteral("image");
    }
    return QStringLiteral("folder");
}

Sequence::Kind kindFromName(const QString &s)
{
    if (s == QLatin1String("archive")) return Sequence::Kind::Archive;
    if (s == QLatin1String("image"))   return Sequence::Kind::Image;
    return Sequence::Kind::Folder;
}

// 文件名里不允许的字符（Windows 的集合 + 控制字符）⇒ `_`
QString sanitized(const QString &title)
{
    QString out = title;
    for (QChar &c : out) {
        if (c.unicode() < 0x20 || QStringLiteral("/\\:*?\"<>|").contains(c))
            c = QLatin1Char('_');
    }
    out = out.trimmed();
    if (out.isEmpty())
        out = QStringLiteral("progress");
    return out;
}

QJsonObject rowToJson(const Sequence::Row &r)
{
    QJsonObject o;
    o.insert(QStringLiteral("ord"), r.ord);
    o.insert(QStringLiteral("parentOrd"), r.parentOrd);
    o.insert(QStringLiteral("kind"), int(r.kind));
    o.insert(QStringLiteral("label"), r.label);
    o.insert(QStringLiteral("pageIndex"), r.pageIndex);
    o.insert(QStringLiteral("size"), double(r.size));
    o.insert(QStringLiteral("mtime"), double(r.mtime));
    o.insert(QStringLiteral("width"), r.width);
    o.insert(QStringLiteral("height"), r.height);
    o.insert(QStringLiteral("firstIndex"), r.firstIndex);
    o.insert(QStringLiteral("lastIndex"), r.lastIndex);
    //   ★ 加密压缩包（2026-11-03）：容器行的密码。按**条目**存（用户要求：即使勾了
    //     "后续使用相同密码"，也逐个容器写，不搞一份通用密码）。明文 —— 用户已确认。
    if (!r.password.isEmpty())
        o.insert(QStringLiteral("password"), r.password);
    return o;
}

Sequence::Row rowFromJson(const QJsonObject &o)
{
    Sequence::Row r;
    r.ord        = o.value(QStringLiteral("ord")).toInt();
    r.parentOrd  = o.value(QStringLiteral("parentOrd")).toInt(-1);
    r.kind       = Sequence::Kind(o.value(QStringLiteral("kind")).toInt());
    r.label      = o.value(QStringLiteral("label")).toString();
    r.pageIndex  = o.value(QStringLiteral("pageIndex")).toInt(-1);
    r.size       = qint64(o.value(QStringLiteral("size")).toDouble());
    r.mtime      = qint64(o.value(QStringLiteral("mtime")).toDouble());
    r.width      = o.value(QStringLiteral("width")).toInt();
    r.height     = o.value(QStringLiteral("height")).toInt();
    r.firstIndex = o.value(QStringLiteral("firstIndex")).toInt(-1);
    r.lastIndex  = o.value(QStringLiteral("lastIndex")).toInt(-1);
    r.password   = o.value(QStringLiteral("password")).toString();
    return r;
}

// 元数据段：逐行 `key value…`，遇到 `---` 结束；返回 rows 在字节流里的起点
bool parseHead(const QByteArray &bytes, Meta *m, int *rowsOffset, QString *err)
{
    int pos = 0;
    bool sawMagic = false;
    while (pos < bytes.size()) {
        int nl = bytes.indexOf('\n', pos);
        if (nl < 0) {                       // 元数据段没结束（文件不完整 / 被截断）
            if (err) *err = QStringLiteral("进度文件不完整（元数据段没有结束标记）");
            return false;
        }
        const QByteArray raw = bytes.mid(pos, nl - pos);
        pos = nl + 1;
        if (raw == QByteArray(kSeparator)) {
            if (!sawMagic) {
                if (err) *err = QStringLiteral("不是进度文件（缺少 prsave 头）");
                return false;
            }
            if (rowsOffset) *rowsOffset = pos;
            return true;
        }
        const int sp = raw.indexOf(' ');
        const QByteArray key = (sp < 0) ? raw : raw.left(sp);
        const QString value = (sp < 0) ? QString()
                                       : QString::fromUtf8(raw.mid(sp + 1));

        if (key == QByteArray(kMagic)) {
            bool ok = false;
            const int v = value.toInt(&ok);
            if (!ok) {
                if (err) *err = QStringLiteral("不是进度文件（版本行看不懂）");
                return false;
            }
            if (v < 1 || v > kVersion) {
                if (err) *err = QStringLiteral("进度文件版本不认识（v%1）").arg(v);
                return false;
            }
            m->version = v;
            sawMagic = true;
        } else if (key == "savedAt") {
            m->savedAt = value.toLongLong();
        } else if (key == "title") {
            m->title = unescaped(value);
        } else if (key == "source") {
            m->sourcePath = unescaped(value);
        } else if (key == "password") {                  // ★ 源本身（外层容器）的密码
            m->sourcePassword = unescaped(value);
        } else if (key == "kind") {
            m->sourceKind = kindFromName(value);
        } else if (key == "mtime") {
            m->sourceMtime = value.toLongLong();
        } else if (key == "scan") {
            const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() == 3) {
                m->hidden = parts.at(0) != QLatin1String("0");
                m->depth = parts.at(1).toInt();
                m->innerArchiveMaxMb = parts.at(2).toLongLong();
            }
        } else if (key == "layout") {
            m->layout = value.toInt();
        } else if (key == "chain") {
            const QJsonDocument d = QJsonDocument::fromJson(value.toUtf8());
            if (d.isArray()) {
                m->chain.clear();
                for (const QJsonValue &v : d.array())
                    m->chain << v.toString();
            }
        } else if (key == "page") {
            m->page = value.toInt();
        } else if (key == "progress") {
            const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() == 2) {
                m->done = parts.at(0).toInt();
                m->total = parts.at(1).toInt();
            }
        } else if (key == "rows") {
            m->rowCount = value.toInt();
        }
        // 不认识的键：忽略（前向兼容）
    }
    if (err) *err = QStringLiteral("进度文件不完整（没有元数据段结束标记）");
    return false;
}

// v1（2026-10-31）：整个文件是一个 JSON 对象，字段都在顶层
bool readV1(const QByteArray &bytes, Data *out, QString *err)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = QStringLiteral("不是有效的进度文件");
        return false;
    }
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("format")).toString() != QLatin1String("prsave")) {
        if (err) *err = QStringLiteral("不是进度文件");
        return false;
    }
    const int version = o.value(QStringLiteral("version")).toInt(0);
    if (version < 1 || version > 1) {
        if (err) *err = QStringLiteral("进度文件版本不认识（v%1）").arg(version);
        return false;
    }

    Data d;
    d.meta.version = version;
    d.meta.savedAt = qint64(o.value(QStringLiteral("savedAt")).toDouble());
    d.meta.title = o.value(QStringLiteral("title")).toString();

    const QJsonObject source = o.value(QStringLiteral("source")).toObject();
    d.meta.sourcePath = source.value(QStringLiteral("path")).toString();
    d.meta.sourceKind = Sequence::Kind(source.value(QStringLiteral("kind")).toInt());
    d.meta.sourceMtime = qint64(source.value(QStringLiteral("mtime")).toDouble());

    const QJsonObject scan = o.value(QStringLiteral("scan")).toObject();
    d.meta.hidden = scan.value(QStringLiteral("hidden")).toBool();
    d.meta.depth = scan.value(QStringLiteral("depth")).toInt(5);
    d.meta.innerArchiveMaxMb = qint64(scan.value(QStringLiteral("innerArchiveMaxMb")).toDouble(512));

    d.meta.layout = o.value(QStringLiteral("layout")).toInt(0);

    const QJsonObject position = o.value(QStringLiteral("position")).toObject();
    for (const QJsonValue &v : position.value(QStringLiteral("chain")).toArray())
        d.meta.chain << v.toString();
    d.meta.page = position.value(QStringLiteral("page")).toInt(0);

    const QJsonObject progress = o.value(QStringLiteral("progress")).toObject();
    d.meta.done = progress.value(QStringLiteral("done")).toInt(0);
    d.meta.total = progress.value(QStringLiteral("total")).toInt(0);

    const QJsonArray rows = o.value(QStringLiteral("rows")).toArray();
    d.rows.reserve(rows.size());
    for (const QJsonValue &v : rows)
        d.rows.append(rowFromJson(v.toObject()));
    d.meta.rowCount = d.rows.size();

    if (d.meta.sourcePath.isEmpty()) {
        if (err) *err = QStringLiteral("进度文件里没有来源路径");
        return false;
    }
    if (d.rows.isEmpty()) {
        if (err) *err = QStringLiteral("进度文件里没有条目");
        return false;
    }
    *out = d;
    return true;
}

} // namespace

QString hash8(const QString &sourcePath)
{
    //   ★ 统一成**绝对路径**（相对 ⇒ 绝对，`.`/`..` 折掉），但**不解析符号链接**——
    //     符号链接是用户自己的选择，不替他归到"原始路径"（用户 2026-11-08）。
    const QString key = QDir::cleanPath(QFileInfo(sourcePath).absoluteFilePath());
    const QByteArray hex = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(hex.left(8));
}

QString titleOf(const QString &sourcePath)
{
    const QFileInfo fi(sourcePath);
    const QString name = fi.fileName();          // 文件夹名 / 压缩包文件名（含扩展名）
    if (!name.isEmpty())
        return name;
    return QFileInfo(fi.absolutePath()).fileName();
}

QString fileNameFor(const QString &title, const QString &sourcePath)
{
    return sanitized(title) + QLatin1Char('-') + hash8(sourcePath)
           + QLatin1Char('.') + QLatin1String(kSuffix);
}

bool isProgressFile(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String(kSuffix), Qt::CaseInsensitive) == 0;
}

bool write(const QString &filePath, const Data &d, QString *err)
{
    QByteArray head;
    head += QByteArray(kMagic) + ' ' + QByteArray::number(kVersion) + '\n';
    head += "savedAt " + QByteArray::number(d.meta.savedAt) + '\n';
    head += "title " + escaped(d.meta.title).toUtf8() + '\n';
    head += "source " + escaped(d.meta.sourcePath).toUtf8() + '\n';
    if (!d.meta.sourcePassword.isEmpty())                 // ★ 加密压缩包（源本身）
        head += "password " + escaped(d.meta.sourcePassword).toUtf8() + '\n';
    head += "kind " + kindName(d.meta.sourceKind).toUtf8() + '\n';
    head += "mtime " + QByteArray::number(d.meta.sourceMtime) + '\n';
    head += "scan " + QByteArray(d.meta.hidden ? "1" : "0") + ' '
            + QByteArray::number(d.meta.depth) + ' '
            + QByteArray::number(double(d.meta.innerArchiveMaxMb)) + '\n';
    head += "layout " + QByteArray::number(d.meta.layout) + '\n';
    head += "chain " + QJsonDocument(QJsonArray::fromStringList(d.meta.chain)).toJson(QJsonDocument::Compact) + '\n';
    head += "page " + QByteArray::number(d.meta.page) + '\n';
    head += "progress " + QByteArray::number(d.meta.done) + ' '
            + QByteArray::number(d.meta.total) + '\n';
    head += "rows " + QByteArray::number(d.rows.size()) + '\n';
    head += QByteArray(kSeparator) + '\n';

    QJsonArray rows;
    for (const Sequence::Row &r : d.rows)
        rows.append(rowToJson(r));
    const QByteArray tail = QJsonDocument(rows).toJson(QJsonDocument::Indented);

    const QString dir = QFileInfo(filePath).absolutePath();
    if (!QDir().mkpath(dir)) {
        if (err) *err = QStringLiteral("建不了目录：%1").arg(dir);
        return false;
    }

    //   ★ 原子替换：写临时文件 → commit 改名。半截文件不会被读到。
    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QStringLiteral("写不了 %1").arg(filePath);
        return false;
    }
    if (f.write(head + tail) < 0 || !f.commit()) {
        if (err) *err = QStringLiteral("写入失败：%1").arg(filePath);
        return false;
    }
    return true;
}

bool read(const QString &filePath, Data *out, QString *err)
{
    if (!out) {
        if (err) *err = QStringLiteral("内部错误");
        return false;
    }
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("读不了 %1").arg(filePath);
        return false;
    }
    const QByteArray bytes = f.readAll();

    //   v1：整个文件是一个 JSON 对象
    if (!bytes.startsWith(QByteArray(kMagic) + ' '))
        return readV1(bytes, out, err);

    Data d;
    int rowsOffset = -1;
    if (!parseHead(bytes, &d.meta, &rowsOffset, err))
        return false;

    const QJsonDocument doc = QJsonDocument::fromJson(bytes.mid(rowsOffset));
    if (!doc.isArray()) {
        if (err) *err = QStringLiteral("进度文件的行数据坏了：%1").arg(QFileInfo(filePath).fileName());
        return false;
    }
    const QJsonArray rows = doc.array();
    d.rows.reserve(rows.size());
    for (const QJsonValue &v : rows)
        d.rows.append(rowFromJson(v.toObject()));
    d.meta.rowCount = d.rows.size();

    if (d.meta.sourcePath.isEmpty()) {
        if (err) *err = QStringLiteral("进度文件里没有来源路径");
        return false;
    }
    if (d.rows.isEmpty()) {
        if (err) *err = QStringLiteral("进度文件里没有条目");
        return false;
    }
    *out = d;
    return true;
}

bool readMeta(const QString &filePath, Meta *out, QString *err)
{
    if (!out) {
        if (err) *err = QStringLiteral("内部错误");
        return false;
    }
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("读不了 %1").arg(filePath);
        return false;
    }
    //   ★ 只读前面一小截（元数据段通常 < 1 KB）：**不解析 rows**
    const QByteArray head = f.read(kMetaReadLimit);
    if (!head.startsWith(QByteArray(kMagic) + ' ')) {
        if (err) *err = QStringLiteral("不是 v2 进度文件");   // v1 ⇒ 调用方退回整份读
        return false;
    }
    Meta m;
    if (!parseHead(head, &m, nullptr, err))
        return false;
    if (m.sourcePath.isEmpty()) {
        if (err) *err = QStringLiteral("进度文件里没有来源路径");
        return false;
    }
    *out = m;
    return true;
}

} // namespace progressfile

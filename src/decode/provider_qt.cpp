#include "provider_qt.h"

#include <QBuffer>
#include <QImageReader>

namespace decode {
namespace {

class QtProvider final : public Provider
{
public:
    QString name() const override { return QStringLiteral("qt"); }
    int     rank() const override { return 10; }

    //   ★ 唯一的信息来源：Qt 自己报的插件列表（运行时）。
    //     注意 `QImageReader` 认的是**内容**不是后缀，所以这张表只用于"收录"，
    //     同名不同内容（`.jpg` 里其实是 PNG）照样能解。
    void probe() override
    {
        m_suffixes.clear();
        const QList<QByteArray> fmts = QImageReader::supportedImageFormats();
        for (const QByteArray &f : fmts) m_suffixes << QString::fromLatin1(f).toLower();
        m_suffixes.removeDuplicates();
        m_suffixes.sort();
    }

    bool        available() const override { return true; }
    QStringList advertisedSuffixes() const override { return m_suffixes; }

    QImage decode(const Source &src, int page, QString *why) override
    {
        QImageReader r;
        QBuffer buf;
        if (!open(src, r, buf, why)) return {};
        //   方向：仍由 `QImageReader` 按 EXIF 自己转（与阶段 A 之前逐位一致）。
        //   ★ 等 WIC / gdk 接入（阶段 B）时再统一成"provider 只报方向、外层施加"：
        //     那两种 API 没有 autoTransform 的等价物，届时一起改，避免现在先改一半。
        r.setAutoTransform(true);
        if (page > 0) r.jumpToImage(page);
        const QImage img = r.read();
        if (img.isNull() && why) *why = r.errorString();
        return img;
    }

    QSize querySize(const Source &src, int page, QString *fmt, int *frames) override
    {
        QImageReader r;
        QBuffer buf;
        if (!open(src, r, buf, nullptr)) return {};
        if (page > 0) r.jumpToImage(page);
        if (fmt) *fmt = QString::fromLatin1(r.format());
        if (frames) {
            const int n = r.imageCount();
            *frames = n > 0 ? n : 1;
        }
        return r.size();          // 只读头，不解码
    }

    //   ★ 覆盖默认实现：交给 Qt 的 `setScaledSize`，JPEG/PNG 这类会走**解码器自己的降采样**
    //     （libjpeg 的 DCT 缩放 = 1/2、1/4、1/8），比"解全图再缩"快几倍、峰值内存也小得多。
    //     这就是那个 thumbnail 钩子的全部意义 —— 缩略图**策略**仍然在调用方手里。
    QImage thumbnail(const Source &src, const QSize &target) override
    {
        if (!target.isValid()) return {};
        QImageReader r;
        QBuffer buf;
        if (!open(src, r, buf, nullptr)) return {};
        r.setAutoTransform(true);
        r.setScaledSize(target);
        return r.read();
    }

private:
    static bool open(const Source &src, QImageReader &r, QBuffer &buf, QString *why)
    {
        if (src.isDisk) {
            r.setFileName(src.path);
            return true;
        }
        if (src.bytes.isEmpty()) {
            if (why) *why = QStringLiteral("no bytes");
            return false;
        }
        buf.setData(src.bytes);
        if (!buf.open(QIODevice::ReadOnly)) {
            if (why) *why = QStringLiteral("buffer open failed");
            return false;
        }
        r.setDevice(&buf);
        return true;
    }

    QStringList m_suffixes;
};

}  // namespace

Provider *makeQtProvider()
{
    return new QtProvider;
}

}  // namespace decode

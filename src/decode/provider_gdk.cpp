#include "provider_gdk.h"

#if defined(Q_OS_LINUX)

#  include <QFile>
#  include <QImage>
#  include <QStringList>

#  include <dlfcn.h>

#  include <cstring>

//   ★ 这里**不包含任何 glib / gdk-pixbuf 头文件**：项目不为这一路增加编译期依赖。
//     需要的类型按 ABI 自己声明（glib 的这几个类型在 ABI 上极稳定）。
namespace decode {
namespace {

//   glib 的 GSList：`{ gpointer data; GSList *next; }`
struct GSList_
{
    void      *data = nullptr;
    GSList_   *next = nullptr;
};

using guchar  = unsigned char;
using gchar   = char;
using gboolean = int;
using gsize   = size_t;

//   GdkPixbuf / GdkPixbufLoader / GError 都当**不透明指针**用
using FnGetFormats     = GSList_ *(*)();
using FnFormatExts     = gchar **(*)(void *);
using FnFormatName     = gchar *(*)(void *);
using FnNewFromFile    = void *(*)(const gchar *, void **);
using FnLoaderNew      = void *(*)();
using FnLoaderWrite    = gboolean (*)(void *, const guchar *, gsize, void **);
using FnLoaderClose    = gboolean (*)(void *, void **);
using FnLoaderPixbuf   = void *(*)(void *);
using FnFileInfo       = void *(*)(const gchar *, int *, int *);
using FnGetWidth       = int (*)(const void *);
using FnGetHeight      = int (*)(const void *);
using FnGetChannels    = int (*)(const void *);
using FnGetRowstride   = int (*)(const void *);
using FnGetHasAlpha    = gboolean (*)(const void *);
using FnGetPixels      = const guchar *(*)(const void *);
using FnObjUnref       = void (*)(void *);
using FnErrorFree      = void (*)(void *);
using FnStrfreev       = void (*)(gchar **);

//   **一次加载，进程内复用**。任何一步失败 ⇒ 这一路不存在（不报错、不影响别的 provider）。
struct Libs
{
    void *gdk  = nullptr;
    void *gobj = nullptr;
    void *glib = nullptr;

    FnGetFormats   getFormats = nullptr;
    FnFormatExts   formatExts = nullptr;
    FnNewFromFile  newFromFile = nullptr;
    FnLoaderNew    loaderNew = nullptr;
    FnLoaderWrite  loaderWrite = nullptr;
    FnLoaderClose  loaderClose = nullptr;
    FnLoaderPixbuf loaderPixbuf = nullptr;
    FnFileInfo     fileInfo = nullptr;
    FnGetWidth     getWidth = nullptr;
    FnGetHeight    getHeight = nullptr;
    FnGetChannels  getChannels = nullptr;
    FnGetRowstride getRowstride = nullptr;
    FnGetHasAlpha  getHasAlpha = nullptr;
    FnGetPixels    getPixels = nullptr;
    FnObjUnref     objUnref = nullptr;
    FnErrorFree    errorFree = nullptr;
    FnStrfreev     strfreev = nullptr;

    bool ok = false;
};

template <typename T>
T sym(void *lib, const char *name)
{
    return reinterpret_cast<T>(dlsym(lib, name));
}

Libs loadLibs()
{
    Libs L;
    //   ★ 只在真需要时才 dlopen；失败就是"这台机器没有 GNOME 栈"，不是错误。
    L.gdk  = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    L.gobj = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    L.glib = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!L.gdk || !L.gobj || !L.glib) return L;

    L.getFormats   = sym<FnGetFormats>(L.gdk, "gdk_pixbuf_get_formats");
    L.formatExts   = sym<FnFormatExts>(L.gdk, "gdk_pixbuf_format_get_extensions");
    L.newFromFile  = sym<FnNewFromFile>(L.gdk, "gdk_pixbuf_new_from_file");
    L.loaderNew    = sym<FnLoaderNew>(L.gdk, "gdk_pixbuf_loader_new");
    L.loaderWrite  = sym<FnLoaderWrite>(L.gdk, "gdk_pixbuf_loader_write");
    L.loaderClose  = sym<FnLoaderClose>(L.gdk, "gdk_pixbuf_loader_close");
    L.loaderPixbuf = sym<FnLoaderPixbuf>(L.gdk, "gdk_pixbuf_loader_get_pixbuf");
    L.fileInfo     = sym<FnFileInfo>(L.gdk, "gdk_pixbuf_get_file_info");
    L.getWidth     = sym<FnGetWidth>(L.gdk, "gdk_pixbuf_get_width");
    L.getHeight    = sym<FnGetHeight>(L.gdk, "gdk_pixbuf_get_height");
    L.getChannels  = sym<FnGetChannels>(L.gdk, "gdk_pixbuf_get_n_channels");
    L.getRowstride = sym<FnGetRowstride>(L.gdk, "gdk_pixbuf_get_rowstride");
    L.getHasAlpha  = sym<FnGetHasAlpha>(L.gdk, "gdk_pixbuf_get_has_alpha");
    L.getPixels    = sym<FnGetPixels>(L.gdk, "gdk_pixbuf_get_pixels");
    L.objUnref     = sym<FnObjUnref>(L.gobj, "g_object_unref");
    L.errorFree    = sym<FnErrorFree>(L.glib, "g_error_free");
    L.strfreev     = sym<FnStrfreev>(L.glib, "g_strfreev");

    L.ok = L.getFormats && L.formatExts && L.newFromFile && L.loaderNew && L.loaderWrite
           && L.loaderClose && L.loaderPixbuf && L.fileInfo && L.getWidth && L.getHeight
           && L.getChannels && L.getRowstride && L.getHasAlpha && L.getPixels && L.objUnref;
    return L;
}

const Libs &libs()
{
    static const Libs L = loadLibs();
    return L;
}

//   GdkPixbuf → QImage。
//   ★ 布局：gdk-pixbuf 是 **RGBA 逐字节**（R,G,B,A），正对 `Format_RGBA8888`；
//     别拿它当 `Format_ARGB32`（那是内存里的 B,G,R,A）—— 那样会红蓝互换。
//     降位深/转 ARGB32 由 `imageread` 的 `toDisplayable()` 统一做（一处就够）。
QImage toQImage(const Libs &L, const void *pb)
{
    if (!pb) return {};
    const int w = L.getWidth(pb);
    const int h = L.getHeight(pb);
    const int n = L.getChannels(pb);
    const int stride = L.getRowstride(pb);
    const guchar *pixels = L.getPixels(pb);
    if (w <= 0 || h <= 0 || !pixels || (n != 3 && n != 4)) return {};

    QImage img(w, h, QImage::Format_RGBA8888);
    if (img.isNull()) return {};
    for (int y = 0; y < h; ++y) {
        const guchar *src = pixels + qsizetype(y) * stride;
        uchar *dst = img.scanLine(y);
        if (n == 4) {
            std::memcpy(dst, src, size_t(w) * 4);
        } else {
            for (int x = 0; x < w; ++x) {
                dst[x * 4 + 0] = src[x * 3 + 0];
                dst[x * 4 + 1] = src[x * 3 + 1];
                dst[x * 4 + 2] = src[x * 3 + 2];
                dst[x * 4 + 3] = 0xFF;
            }
        }
    }
    return img;
}

class GdkProvider final : public Provider
{
public:
    QString name() const override { return QStringLiteral("gdk"); }
    int     rank() const override { return 20; }

    //   ★ 唯一的信息来源：**已装 loader 自己声明的扩展名**。
    void probe() override
    {
        m_suffixes.clear();
        const Libs &L = libs();
        if (!L.ok) return;
        for (GSList_ *it = L.getFormats(); it; it = it->next) {
            if (!it->data) continue;
            gchar **exts = L.formatExts(it->data);
            if (!exts) continue;
            for (int i = 0; exts[i]; ++i) {
                const QString s = QString::fromLatin1(exts[i]).trimmed().toLower();
                //   ★ 有的 loader 声明的"扩展名"自带点（如 `svg.gz`）：那不是单一后缀，
                //     放进白名单只会污染列表（`isImageSuffix("svg.gz")` 永远不会命中）。
                if (s.isEmpty() || s.contains(QLatin1Char('.')) || s.contains(QLatin1Char('*'))) continue;
                if (!m_suffixes.contains(s)) m_suffixes << s;
            }
            if (L.strfreev) L.strfreev(exts);
        }
        m_suffixes.sort();
    }

    bool available() const override { return libs().ok; }
    QStringList advertisedSuffixes() const override { return m_suffixes; }

    QImage decode(const Source &src, int page, QString *why) override
    {
        Q_UNUSED(page);
        const Libs &L = libs();
        if (!L.ok) {
            if (why) *why = QStringLiteral("gdk: unavailable");
            return {};
        }

        if (src.isDisk) {
            //   ★ 文件名必须是**本地 8-bit 编码**（glib 的 filename encoding），
            //     不能用 UTF-8 —— 否则非 UTF-8 locale 下的中文路径打不开。
            void *pb = L.newFromFile(QFile::encodeName(src.path).constData(), nullptr);
            if (!pb) {
                if (why) *why = QStringLiteral("gdk: open failed");
                return {};
            }
            const QImage img = toQImage(L, pb);
            L.objUnref(pb);
            if (img.isNull() && why) *why = QStringLiteral("gdk: unsupported layout");
            return img;
        }

        if (src.bytes.isEmpty()) {
            if (why) *why = QStringLiteral("gdk: no bytes");
            return {};
        }
        //   包内成员：走 loader（gdk-pixbuf 没有"从内存开文件"的公开 API）
        void *loader = L.loaderNew();
        if (!loader) return {};
        QImage out;
        void *err = nullptr;
        if (L.loaderWrite(loader, reinterpret_cast<const guchar *>(src.bytes.constData()),
                          gsize(src.bytes.size()), &err)) {
            if (L.loaderClose(loader, &err)) out = toQImage(L, L.loaderPixbuf(loader));
        }
        if (err && L.errorFree) L.errorFree(err);
        L.objUnref(loader);
        if (out.isNull() && why) *why = QStringLiteral("gdk: decode failed");
        return out;
    }

    //   尺寸：磁盘文件用 `gdk_pixbuf_get_file_info`（**只读头**，很便宜）。
    //   包内成员没有对应 API ⇒ 不走这一路（交给链上下一个 provider）。
    QSize querySize(const Source &src, int page, QString *fmt, int *frames) override
    {
        Q_UNUSED(page);
        if (fmt) fmt->clear();
        if (frames) *frames = 1;
        const Libs &L = libs();
        if (!L.ok || !src.isDisk) return {};
        int w = 0, h = 0;
        if (!L.fileInfo(QFile::encodeName(src.path).constData(), &w, &h)) return {};
        return (w > 0 && h > 0) ? QSize(w, h) : QSize();
    }

private:
    QStringList m_suffixes;
};

}  // namespace

Provider *makeGdkProvider()
{
    if (!libs().ok) return nullptr;      // 没有 GNOME 栈 ⇒ 这一路不存在
    return new GdkProvider;
}

}  // namespace decode

#else   // 非 Linux：这一路不存在

namespace decode {
Provider *makeGdkProvider()
{
    return nullptr;
}
}  // namespace decode

#endif

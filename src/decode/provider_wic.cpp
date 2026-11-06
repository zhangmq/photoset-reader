#include "provider_wic.h"

#if defined(Q_OS_WIN)

//   ★ 必须在 Qt 头之前包含 windows.h / wincodec.h（并且避开 Qt 的 min/max 宏冲突，
//     用 NOMINMAX + WIN32_LEAN_AND_MEAN），否则 MSVC 下会出现 min/max 与 Qt 的模板打架。
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <objbase.h>
#  include <wincodec.h>

#  include <QStringList>
#  include <QVarLengthArray>

namespace decode {
namespace {

template <typename T>
void safeRelease(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

//   **每线程**一份 COM + 工厂：解码跑在 worker 线程，COM 必须按线程初始化。
//   · 主线程：Qt 的 Windows 平台插件已经用 STA（OleInitialize）初始化过 COM
//     ⇒ `CoInitializeEx(MTA)` 会返回 `RPC_E_CHANGED_MODE`；那不是错误，COM 照样可用，
//       只是用 STA —— 所以这里**接受** S_OK / S_FALSE / RPC_E_CHANGED_MODE 三种结果。
//   · worker 线程：由我们创建，MTA 最省事（WIC 官方推荐 MTA）。
struct ComThread
{
    ComThread()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_ownRef = (hr == S_OK || hr == S_FALSE);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&factory));
        }
    }
    ~ComThread()
    {
        safeRelease(factory);
        if (m_ownRef) CoUninitialize();
    }

    IWICImagingFactory *factory = nullptr;

private:
    bool m_ownRef = false;
};

//   ★ thread_local：线程退出时自动释放（COM 的引用计数在该线程上平衡）
thread_local ComThread g_com;

QString hrText(HRESULT hr)
{
    return QStringLiteral("wic: hr=0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

//   从 WIC 报的扩展名串里取后缀。
//   ★★ 实测（2026-11-06）：WIC 报的是**逗号分隔、不带空格**的一串，例如
//      `.bmp,.dib,.rle` / `.arw,.cr2,.nef,…` / `.heic,.heif,.avif,.avifs`。
//      按空格切会得到"每个编解码器只剩最后一个后缀"（bmp→rle、jpeg→exif、raw→dng），
//      能力表直接少掉一大半 —— 这是个**只看代码发现不了**的坑，必须拿真机枚举对。
//      分隔符按 `,` `;` 空格全都当成分隔（不同编解码器/版本写法不一致）。
void collectSuffixes(const QString &raw, QStringList *out)
{
    QString norm = raw;
    norm.replace(QLatin1Char(','), QLatin1Char(' '));
    norm.replace(QLatin1Char(';'), QLatin1Char(' '));
    const auto parts = norm.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString s : parts) {
        s = s.trimmed().toLower();
        if (s.startsWith(QLatin1String("*."))) s = s.mid(2);      // "*.arw"
        else if (s.startsWith(QLatin1Char('.'))) s = s.mid(1);    // ".arw"
        if (s.isEmpty() || s.contains(QLatin1Char('*'))) continue;
        if (s.contains(QLatin1Char('.'))) continue;               // 不是单一后缀
        if (!out->contains(s)) out->append(s);
    }
}

class WicProvider final : public Provider
{
public:
    QString name() const override { return QStringLiteral("wic"); }
    int     rank() const override { return 20; }

    //   ★ 唯一的信息来源：系统已注册的解码器枚举。
    //     枚举不到任何东西（例如 Server 版没装任何图像扩展）⇒ 这一路就不出现。
    void probe() override
    {
        m_suffixes.clear();
        IWICImagingFactory *f = g_com.factory;
        if (!f) return;

        IEnumUnknown *en = nullptr;
        if (FAILED(f->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &en)) || !en)
            return;

        IUnknown *unk = nullptr;
        ULONG got = 0;
        while (en->Next(1, &unk, &got) == S_OK && got == 1 && unk) {
            IWICComponentInfo *info = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&info))) && info) {
                WICComponentType type = WICAllComponents;
                if (SUCCEEDED(info->GetComponentType(&type)) && type == WICDecoder) {
                    //   ★ 扩展名在 `IWICBitmapDecoderInfo` 上（`IWICComponentInfo` 只有类型/名称）。
                    //     这一步也是"这个编解码器愿意认哪些后缀"的**唯一**来源。
                    IWICBitmapDecoderInfo *dinfo = nullptr;
                    if (SUCCEEDED(info->QueryInterface(IID_PPV_ARGS(&dinfo))) && dinfo) {
                        UINT len = 0;
                        if (SUCCEEDED(dinfo->GetFileExtensions(0, nullptr, &len)) && len > 1) {
                            QVarLengthArray<WCHAR, 256> buf(int(len) + 1);
                            if (SUCCEEDED(dinfo->GetFileExtensions(len, buf.data(), &len)))
                                collectSuffixes(QString::fromWCharArray(buf.constData()), &m_suffixes);
                        }
                        dinfo->Release();
                    }
                }
                info->Release();
            }
            unk->Release();
            unk = nullptr;
        }
        en->Release();

        m_suffixes.removeDuplicates();
        m_suffixes.sort();
    }

    bool        available() const override { return g_com.factory != nullptr; }
    QStringList advertisedSuffixes() const override { return m_suffixes; }

    QImage decode(const Source &src, int page, QString *why) override
    {
        IWICImagingFactory *f = g_com.factory;
        if (!f) {
            if (why) *why = QStringLiteral("wic: no factory");
            return {};
        }

        IWICStream *stream = nullptr;
        IWICBitmapDecoder *dec = nullptr;
        HRESULT hr = E_FAIL;
        if (src.isDisk) {
            hr = f->CreateDecoderFromFilename(reinterpret_cast<LPCWSTR>(src.path.utf16()), nullptr,
                                              GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
        } else if (!src.bytes.isEmpty()) {
            hr = f->CreateStream(&stream);
            if (SUCCEEDED(hr) && stream) {
                hr = stream->InitializeFromMemory(
                    reinterpret_cast<BYTE *>(const_cast<char *>(src.bytes.constData())),
                    UINT(src.bytes.size()));
            }
            if (SUCCEEDED(hr))
                hr = f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
        }
        if (FAILED(hr) || !dec) {
            if (why) *why = hrText(hr);
            safeRelease(stream);
            return {};
        }

        QImage out;
        UINT frames = 0;
        if (SUCCEEDED(dec->GetFrameCount(&frames)) && frames > 0 && UINT(page) < frames) {
            IWICBitmapFrameDecode *frame = nullptr;
            if (SUCCEEDED(dec->GetFrame(UINT(page), &frame)) && frame) {
                UINT w = 0, h = 0;
                IWICFormatConverter *conv = nullptr;
                if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0
                    && SUCCEEDED(f->CreateFormatConverter(&conv)) && conv
                    //   WIC 的 32bppBGRA 与 QImage::Format_ARGB32 的内存布局一致（小端 BGRA）
                    && SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                                  WICBitmapDitherTypeNone, nullptr, 0.0,
                                                  WICBitmapPaletteTypeCustom))) {
                    QImage img(int(w), int(h), QImage::Format_ARGB32);
                    if (!img.isNull()) {
                        const UINT stride = UINT(img.bytesPerLine());
                        if (SUCCEEDED(conv->CopyPixels(nullptr, stride, stride * h, img.bits())))
                            out = img;
                    }
                }
                safeRelease(conv);
                frame->Release();
            }
        }
        safeRelease(dec);
        safeRelease(stream);
        if (out.isNull() && why && why->isEmpty()) *why = QStringLiteral("wic: decode failed");
        return out;
    }

    QSize querySize(const Source &src, int page, QString *fmt, int *frames) override
    {
        IWICImagingFactory *f = g_com.factory;
        if (!f) return {};

        IWICStream *stream = nullptr;
        IWICBitmapDecoder *dec = nullptr;
        HRESULT hr = E_FAIL;
        if (src.isDisk) {
            hr = f->CreateDecoderFromFilename(reinterpret_cast<LPCWSTR>(src.path.utf16()), nullptr,
                                              GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
        } else if (!src.bytes.isEmpty()) {
            hr = f->CreateStream(&stream);
            if (SUCCEEDED(hr) && stream) {
                hr = stream->InitializeFromMemory(
                    reinterpret_cast<BYTE *>(const_cast<char *>(src.bytes.constData())),
                    UINT(src.bytes.size()));
            }
            if (SUCCEEDED(hr))
                hr = f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
        }
        QSize size;
        if (SUCCEEDED(hr) && dec) {
            UINT n = 0;
            if (SUCCEEDED(dec->GetFrameCount(&n))) {
                if (frames) *frames = (n > 0) ? int(n) : 1;
                UINT w = 0, h = 0;
                IWICBitmapFrameDecode *frame = nullptr;
                if (UINT(page) < qMax(1u, n)
                    && SUCCEEDED(dec->GetFrame(UINT(page), &frame)) && frame) {
                    if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0) size = QSize(int(w), int(h));
                    frame->Release();
                }
            }
        }
        if (fmt) fmt->clear();          // WIC 不提供好用的"容器名"，留空即可
        safeRelease(dec);
        safeRelease(stream);
        return size;
    }

private:
    QStringList m_suffixes;
};

}  // namespace

Provider *makeWicProvider()
{
    if (!g_com.factory) return nullptr;      // 没有 WIC ⇒ 这一路不存在
    return new WicProvider;
}

}  // namespace decode

#else   // 非 Windows：这一路不存在

namespace decode {
Provider *makeWicProvider()
{
    return nullptr;
}
}  // namespace decode

#endif

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

#  include <QCoreApplication>
#  include <QStringList>
#  include <QThread>
#  include <QVarLengthArray>

#  include <thread>

#  include "../logging.h"

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
        //   ★ debug 级：万一有调用点落在不该落的线程上（GUI 线程必须是 STA），
        //     这一行能直接把"谁把套间改了"钉出来。
        qCDebug(lcDecode).noquote()
            << QStringLiteral("[cap] wic: CoInitializeEx(MTA) on thread '%1' hr=0x%2")
                   .arg(QThread::currentThread()->objectName())
                   .arg(quint32(hr), 8, 16, QLatin1Char('0'));
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

//   ★★ **必须惰性构造**（2026-11-06 事故的根因）：
//     `thread_local ComThread g_com;` 这种**命名空间作用域**的 thread_local，MSVC 会在线程
//     启动时（主线程就是 `main` 之前）**急切**执行构造函数 ⇒ `CoInitializeEx(MTA)` 在 CRT
//     初始化阶段就把 **GUI 线程钉成 MTA**，于是 Windows 原生文件/文件夹对话框（要求 STA）
//     直接卡死。改成**函数内 static thread_local** ⇒ 只在真正用到 COM 的那个线程上构造。
ComThread &comForThisThread()
{
    static thread_local ComThread t;
    return t;
}

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

    //   ★★ 枚举**不能在本线程（GUI 线程）上初始化 COM**（2026-11-06 事故）：
    //     我们在 `main.cpp` 里启动时调 `probe()`，那时 GUI 线程未必被 Qt 初始化成 STA；
    //     若这里把它 `CoInitializeEx(MTA)` 了，后面 **Windows 原生文件对话框（要求 STA）
    //     就会卡死** —— 症状是"打开文件/文件夹全都卡住"，而命令行传路径却一切正常。
    //     所以：**枚举放到一个短命线程**（它自己 MTA，用完就退），GUI 线程的套间原样不动。
    void probe() override
    {
        m_suffixes.clear();
        m_available = false;

        //   只读诊断（**不会初始化 COM**）：GUI 线程此刻是什么套间？
        APTTYPE at = APTTYPE_CURRENT;
        APTTYPEQUALIFIER q = APTTYPEQUALIFIER_NONE;
        const HRESULT ap = CoGetApartmentType(&at, &q);
        qCDebug(lcDecode).noquote()
            << QStringLiteral("[cap] wic: gui-thread apartment = %1 (hr=0x%2)")
                   .arg(ap == CO_E_NOTINITIALIZED ? QStringLiteral("none(not initialized)")
                        : ap == S_OK && at == APTTYPE_MTA ? QStringLiteral("MTA")
                        : ap == S_OK && at == APTTYPE_STA ? QStringLiteral("STA")
                                                          : QStringLiteral("other"))
                   .arg(quint32(ap), 8, 16, QLatin1Char('0'));

        std::thread worker([this] { enumerateOnThisThread(); });
        worker.join();
    }

    bool        available() const override { return m_available; }

    //   具体组件：WIC 的 COM 工厂在这个系统 DLL 里（各编解码器是它注册的子组件）
    QString     component() const override
    {
        return m_available ? QStringLiteral("windowscodecs.dll") : QString();
    }
    QStringList advertisedSuffixes() const override { return m_suffixes; }

    //   ★★ **GUI 线程上绝不用 COM**（2026-11-06）：Qt 的 Windows 文件/文件夹对话框要求
    //     GUI 线程是 STA；只要我们在那个线程上跑过 `CoInitializeEx(MTA)`，对话框就会卡死
    //     （症状："打开文件/文件夹全都卡住"，命令行传路径却没事）。
    //     预览条那种"可能从 GUI 线程来"的解码，就走一个**临时线程**（自带 MTA），完事就退。
    QImage decode(const Source &src, int page, QString *why) override
    {
        if (isGuiThread()) {
            QImage out;
            QString w;
            std::thread t([&] { out = decodeImpl(src, page, &w); });
            t.join();
            if (why) *why = w;
            return out;
        }
        return decodeImpl(src, page, why);
    }

    QSize querySize(const Source &src, int page, QString *fmt, int *frames) override
    {
        if (isGuiThread()) {
            QSize out;
            QString f;
            int n = 1;
            std::thread t([&] { out = querySizeImpl(src, page, &f, &n); });
            t.join();
            if (fmt) *fmt = f;
            if (frames) *frames = n;
            return out;
        }
        return querySizeImpl(src, page, fmt, frames);
    }

private:
    //   当前线程是不是**主（GUI）线程** —— 是的话一律绕开（见上面 `decode` 的注释）
    static bool isGuiThread()
    {
        QCoreApplication *app = QCoreApplication::instance();
        return app && QThread::currentThread() == app->thread();
    }

    QImage decodeImpl(const Source &src, int page, QString *why)
    {
        IWICImagingFactory *f = comForThisThread().factory;
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

    QSize querySizeImpl(const Source &src, int page, QString *fmt, int *frames)
    {
        IWICImagingFactory *f = comForThisThread().factory;
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
    //   在**当前线程**上初始化 COM 并枚举（只由 `probe()` 在短命线程里调）。
    //   ★ GUI 线程永远不走这里 ⇒ 它的套间（Qt 的 STA）不会被我们改掉。
    void enumerateOnThisThread()
    {
        IWICImagingFactory *f = comForThisThread().factory;      // thread_local：本线程自己的工厂
        if (!f) return;
        m_available = true;

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

    QStringList m_suffixes;
    bool        m_available = false;
};

}  // namespace

Provider *makeWicProvider()
{
    //   ★ 这里**不碰 COM**：`available()` 由 `probe()` 在它自己的线程上确定。
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

#include "heifmeta.h"

#include <QtGlobal>

#include <cstring>

#if defined(Q_OS_WIN)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace heifmeta {
namespace {

//   libheif 的 C ABI（只声明我们用到的那一部分；它这些类型/函数签名多年未变）。
//   ⚠️ `heif_error` 是**按值返回**的三字段结构：平台 ABI 不同，函数指针类型必须与之一致，
//      否则返回值会被读坏（x86-64 上 {int,int,ptr} 走寄存器返回，sizeof 16）。
struct heif_error_
{
    int         code = 0;
    int         subcode = 0;
    const char *message = nullptr;
};
struct heif_context;
struct heif_image_handle;
using heif_item_id = quint32;

using FnContextAlloc   = heif_context *(*)();
using FnContextFree    = void (*)(heif_context *);
using FnReadFromFile   = heif_error_ (*)(heif_context *, const char *, void *);
using FnReadFromMemory = heif_error_ (*)(heif_context *, const void *, size_t, void *);
using FnPrimaryHandle  = heif_error_ (*)(heif_context *, heif_image_handle **);
using FnHandleRelease  = void (*)(heif_image_handle *);
using FnMetaIds        = int (*)(const heif_image_handle *, const char *, heif_item_id *, int);
using FnMetaSize       = size_t (*)(const heif_image_handle *, heif_item_id);
using FnMeta           = heif_error_ (*)(const heif_image_handle *, heif_item_id, void *);
using FnIccSize        = size_t (*)(const heif_image_handle *);
using FnIcc            = heif_error_ (*)(const heif_image_handle *, void *);

struct Lib
{
    bool loaded = false;
    void *handle = nullptr;

    FnContextAlloc   contextAlloc = nullptr;
    FnContextFree    contextFree = nullptr;
    FnReadFromFile   readFromFile = nullptr;
    FnReadFromMemory readFromMemory = nullptr;
    FnPrimaryHandle  primaryHandle = nullptr;
    FnHandleRelease  handleRelease = nullptr;
    FnMetaIds        metaIds = nullptr;
    FnMetaSize       metaSize = nullptr;
    FnMeta           meta = nullptr;
    FnIccSize        iccSize = nullptr;
    FnIcc            icc = nullptr;
};

#if defined(Q_OS_WIN)
void *openLib(const char *name) { return reinterpret_cast<void *>(LoadLibraryA(name)); }
void *symOf(void *h, const char *name)
{
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
}
#else
void *openLib(const char *name) { return dlopen(name, RTLD_LAZY | RTLD_LOCAL); }
void *symOf(void *h, const char *name) { return dlsym(h, name); }
#endif

template <typename T>
T sym(void *h, const char *name)
{
    return reinterpret_cast<T>(symOf(h, name));
}

Lib loadLib()
{
    Lib L;
    //   ★ 按"常见 SONAME / 文件名"依次试：Linux 是 `libheif.so.1`（发行版），
    //     Windows 是 `heif.dll`（vcpkg 装出来的名字就叫这个）。
    const char *names[] = {
#if defined(Q_OS_WIN)
        "heif.dll", "libheif.dll",
#else
        "libheif.so.1", "libheif.so",
#endif
    };
    for (const char *n : names) {
        L.handle = openLib(n);
        if (L.handle) break;
    }
    if (!L.handle) return L;

    L.contextAlloc   = sym<FnContextAlloc>(L.handle, "heif_context_alloc");
    L.contextFree    = sym<FnContextFree>(L.handle, "heif_context_free");
    L.readFromFile   = sym<FnReadFromFile>(L.handle, "heif_context_read_from_file");
    L.readFromMemory = sym<FnReadFromMemory>(L.handle, "heif_context_read_from_memory_without_copy");
    L.primaryHandle  = sym<FnPrimaryHandle>(L.handle, "heif_context_get_primary_image_handle");
    L.handleRelease  = sym<FnHandleRelease>(L.handle, "heif_image_handle_release");
    L.metaIds        = sym<FnMetaIds>(L.handle, "heif_image_handle_get_list_of_metadata_block_IDs");
    L.metaSize       = sym<FnMetaSize>(L.handle, "heif_image_handle_get_metadata_size");
    L.meta           = sym<FnMeta>(L.handle, "heif_image_handle_get_metadata");
    L.iccSize        = sym<FnIccSize>(L.handle, "heif_image_handle_get_raw_color_profile_size");
    L.icc            = sym<FnIcc>(L.handle, "heif_image_handle_get_raw_color_profile");

    L.loaded = L.contextAlloc && L.contextFree && L.readFromFile && L.readFromMemory
               && L.primaryHandle && L.handleRelease && L.metaIds && L.metaSize && L.meta
               && L.iccSize && L.icc;
    return L;
}

const Lib &lib()
{
    static const Lib L = loadLib();
    return L;
}

}  // namespace

bool available()
{
    return lib().loaded;
}

bool read(const QString &path, const QByteArray &bytes, QByteArray *exif, QByteArray *iccOut)
{
    const Lib &L = lib();
    if (!L.loaded) return false;

    heif_context *ctx = L.contextAlloc();
    if (!ctx) return false;

    const QByteArray utf8 = path.toUtf8();
    const heif_error_ err = !path.isEmpty()
        ? L.readFromFile(ctx, utf8.constData(), nullptr)
        : L.readFromMemory(ctx, bytes.constData(), size_t(bytes.size()), nullptr);

    bool got = false;
    if (err.code == 0) {
        heif_image_handle *handle = nullptr;
        if (L.primaryHandle(ctx, &handle).code == 0 && handle) {
            //   Exif：`heif_image_handle_get_metadata()` 交出的前 4 字节是"TIFF 头在块内的偏移"，
            //   跳过它才是 `Exif\0\0…`（HEIF 规范）。
            heif_item_id id = 0;
            if (L.metaIds(handle, "Exif", &id, 1) > 0) {
                const size_t size = L.metaSize(handle, id);
                if (size > 4) {
                    QByteArray blob;
                    blob.resize(int(size));
                    if (L.meta(handle, id, blob.data()).code == 0) {
                        if (exif) *exif = blob.mid(4);
                        got = true;
                    }
                }
            }
            //   ICC：原始（未转色）的 profile，交给上层统一转 sRGB
            const size_t iccLen = L.iccSize(handle);
            if (iccLen > 0) {
                QByteArray blob;
                blob.resize(int(iccLen));
                if (L.icc(handle, blob.data()).code == 0) {
                    if (iccOut) *iccOut = blob;
                    got = true;
                }
            }
            L.handleRelease(handle);
        }
    }
    L.contextFree(ctx);
    return got;
}

}  // namespace heifmeta

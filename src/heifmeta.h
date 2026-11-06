#pragma once

#include <QByteArray>
#include <QString>

// ══ HEIF/AVIF 的**元数据**读取（Exif / ICC），运行期加载 libheif ══════════════
//   ★ 这一路**不解码像素**：像素由 Qt / WIC / gdk / CLI 那些 provider 负责。
//     libheif 在这里的唯一职责是把 BMFF box 表里的 Exif 块与 ICC profile 抠出来，
//     供 OSD 显示（HEIC/AVIF 的这两样都不在 JPEG/TIFF 那套头里）。
//   ★ **全部 symbol 用 dlopen/LoadLibrary 取**：libheif 不再是编译期依赖。
//     装了 ⇒ OSD 有这两行；没装 ⇒ 少这两行，**看图不受任何影响**。
//     代价：Windows 包里可以彻底去掉 `heif.dll`（1.4 MB）。
namespace heifmeta {

//   这个库能不能用（首次调用后缓存结果）
bool available();

//   读一条的 Exif 与 ICC。`path` 非空时按路径打开（libheif 自己 seek，不读全文）；
//   否则把 `bytes`（包内成员的整份字节）交给它。
//   返回：**是否拿到了至少一项**（两项都可能为空，比如别家相机没写 ICC）。
bool read(const QString &path, const QByteArray &bytes, QByteArray *exif, QByteArray *icc);

}  // namespace heifmeta

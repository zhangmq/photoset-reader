#include "natural.h"

#include "decode/capabilities.h"

int naturalCompare(const QString &a, const QString &b)
{
    int i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].isDigit() && b[j].isDigit()) {
            int i2 = i, j2 = j;
            while (i2 < a.size() && a[i2].isDigit()) ++i2;
            while (j2 < b.size() && b[j2].isDigit()) ++j2;
            // 跳过前导零（007 与 7 同值）
            int i3 = i, j3 = j;
            while (i3 < i2 - 1 && a[i3] == QLatin1Char('0')) ++i3;
            while (j3 < j2 - 1 && b[j3] == QLatin1Char('0')) ++j3;
            const int la = i2 - i3, lb = j2 - j3;
            if (la != lb) return la < lb ? -1 : 1;
            for (int k = 0; k < la; ++k)
                if (a[i3 + k] != b[j3 + k]) return a[i3 + k] < b[j3 + k] ? -1 : 1;
            i = i2; j = j2;
        } else {
            const QChar ca = a[i].toLower(), cb = b[j].toLower();
            if (ca != cb) return ca < cb ? -1 : 1;
            ++i; ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

// ══ 白名单 = **运行时能力**（用户 2026-11-06 定：零编译期格式知识）══════════
//   这里原来是**写死的一张格式表**，它有两个方向都错的后果（2026-11-06 实测）：
//     · 列了却解不出：avif / jxl / heic / heif / qoi（Windows 上根本没有对应解码器）
//       ⇒ 文件能进列表、解码必然失败、永远只画占位；
//     · 能解却够不着：jfif / svgz / xbm / xpm / cur（Qt 明明支持，我们没写进去）
//       ⇒ 用户有这些文件就是看不见（`.jfif` 是个正经的 JPEG 扩展名）。
//   现在改为问 `decode::Capabilities`：装了 kimageformats 就自动多出 raw/psd/xcf…，
//   接了系统编解码器就自动多出 heic/jxr…，**一个字符都不用改代码**。
//
//   ★ 这两条保留**函数形态**（不删）：调用点有 6 处（`dirwalker` / `archive` / `sequence`
//     / `main` / `uimodel`），它们表达的是"这条路径要不要按图片收"，不是"我们内建支持什么"。
QStringList imageSuffixes()
{
    return decode::Capabilities::instance().suffixes();
}

bool isImageSuffix(const QString &suffix)
{
    return decode::Capabilities::instance().canDecodeSuffix(suffix);
}

QString nameSortKey(const QString &name, bool isDir)
{
    return isDir ? name + QLatin1Char('/') : name;
}

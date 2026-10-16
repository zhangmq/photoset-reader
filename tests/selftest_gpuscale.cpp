#include "testsupport.h"

#include <QString>
#include <QStringList>

#include "gpuscalecheck.h"
#include "imageprovider.h"

// ══ GPU 缩放渲染路径 × CPU 缩放器：逐像素等价性对照（ib-test-gpuscale）══════
//   原 `image-browser --selftest-gpuscale -d <目录>` 的独立套件版（设计 14 §11.1）。
//   实现本来就在 `src/gpuscalecheck.cpp`（`runGpuScaleCheck`）—— 这里**只做装配 +
//   调用 + 退出码**（装配口径与应用 CLI 一字不差：取素材目录前 2 条原图路径）。
//   ★ **需要真机显示**：offscreen 的 software 后端下 GPU 侧没有可用的 RHI
//     （能过就过；不能过是环境限制，**不改断言**）。

static int runGpuScaleSelfTest(const QString &dir)
{
    ImageProvider provider;
    if (!dir.isEmpty())
        provider.setSource(dir, /*recursive=*/false);

    // 与应用 CLI 的 `--selftest-gpuscale` 同一装配：取前 2 条原图（不取归档内页）
    QStringList paths;
    for (int i = 0; i < qMin(2, provider.count()); ++i) {
        const QString p = provider.absPathAt(i);
        if (!p.isEmpty()) paths << p;
    }
    if (paths.isEmpty())
        return testsupport::skip(QStringLiteral("gpucheck"),
                                 QStringLiteral("没有素材：请用 -d <目录> 指定（例如 -d .testdata/vol01）"));

    // 套件外部环境（`QT_QPA_PLATFORM=offscreen` / 真机 `DISPLAY=:0`）由调用方决定。
    // ★ T0 收尾批：`runGpuScaleCheck` 的 rc=2 = **建不出任何 GL 上下文**（本环境没有 GPU
    //   设备节点）⇒ 是**环境跳过**（77），不是断言失败；rc=3（RHI 管线不可用）**仍是失败**
    //   —— AGENTS.md 明确"管线创建失败"必须真机暴露，不许被跳过掩盖。
    const int rc = runGpuScaleCheck(paths, /*repeats=*/3);
    if (rc == 2)
        return testsupport::skip(QStringLiteral("gpucheck"),
                                 QStringLiteral("本环境建不出 GL 上下文（真机才有 GPU 设备节点）"));
    return rc;
}

int main(int argc, char *argv[])
{
    // `-d <目录>` 由 argv 透传（与旧 CLI 同一形状；设计 14 §11）
    const QString dir = testsupport::dirArg(argc, argv);
    return testsupport::suiteMain(argc, argv, [&] { return runGpuScaleSelfTest(dir); });
}

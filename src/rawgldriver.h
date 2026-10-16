#pragma once

#include <QImage>
#include <QString>

#include "gpuscale.h"

// ══ 自检专用的 **raw GL 驱动**（`gpuscalecheck.cpp` 的兜底路径）══════════════════
//
// ## 为什么会有这个文件
//   离屏自检想跑的是**产品那条 QRhi 管线**（`gpuscale::Pipeline`）。但 QRhi 的 OpenGL
//   后端要一个 `QOpenGLContext`，而 Qt 的 GL 栈是**绑在 QPA 平台**上的：
//     · `QT_QPA_PLATFORM=offscreen` 只实现 GLX（要连 X server）；
//     · `minimalegl` 需要 EGL **window surface**（surfaceless 平台给不出）；
//     · `QNativeInterface::QEGLContext::fromNative()` 要求 QPA 平台自身是 EGL 平台
//       （实现里 `dynamic_cast` 平台集成，不是就直接返回 nullptr，见 libQt6Gui 反汇编）。
//   所以「无 /dev/dri、无 X、无 EGL-QPA」的环境里，**QRhi 建不出来**（真机上 Qt Quick
//   自己会建，生产路径不受影响）。
//
//   于是自检退一步：用 EGL surfaceless 自己建 GL 上下文（真 GL，只是栅格器在 CPU 上，
//   Mesa llvmpipe），**执行同一份 qsb 烘焙 shader** + **同一份 Plan/passUniformsFor**，
//   只把「谁来发 3 个 draw」换成这 40 行 GL 调用。
//   ⇒ 它验证的是**算法与参数**；**QRhi 资源路径本身**必须在有设备节点的机器上
//     用 `--selftest-gpuscale` 验证（见 docs/research/12 的口径限制）。
//
//   本文件**只被自检代码引用**，生产渲染路径不碰它。
namespace rawgl {

class Driver
{
public:
    // 失败返回 nullptr（原因写进 *err）。成功时内部持有 EGL surfaceless 上下文并 makeCurrent。
    static Driver *create(QString *err);
    ~Driver();

    QString rendererInfo() const { return m_info; }

    // 把 src 按 plan 跑完整管线并回读（RGBA8888）。失败返回 false（*err 给原因）。
    //   ★ flipY 由调用方给（GL 上垂直趟要翻转索引，见 gpuscale.h 的说明）；
    //     回读后的 QImage 是**自上而下**的（GL 的 glReadPixels 行序已按此处理）。
    bool render(const QImage &src32, const gpuscale::Plan &plan, QImage *out, QString *err);

    // 诊断（`PHOTOSET_RAWGL_DEBUG=1` 时打印 program/UBO/采样器信息）
    void dumpProgramInfo();

private:
    Driver() = default;
    bool ensure(const gpuscale::Plan &plan, QString *err);
    bool createProgramsAndBuffers(QString *err);
    void release();

    void *m_ctx = nullptr;          // headlessgl::Context*
    QString m_info;

    unsigned m_progLin = 0, m_progRes = 0, m_progArH = 0, m_progArV = 0;
    unsigned m_srcTex = 0, m_linTex = 0, m_hTex = 0, m_hMin = 0, m_hMax = 0, m_outTex = 0;
    unsigned m_boundsH = 0, m_boundsV = 0;   // 抽头边界（CPU 算好，RGBA32F 单行）
    unsigned m_fboLin = 0, m_fboH = 0, m_fboHAr = 0, m_fboOut = 0;
    unsigned m_vao = 0, m_vbo = 0;
    unsigned m_ubo = 0;
    // ★ qsb 的 GLSL 输出把 uniform 块**展平成普通 struct uniform**（`struct buf{...}; uniform buf _ID;`），
    //   QRhi 的 GL 后端就是按 `structName.成员` 逐个设 uniform 的（qrhigles2.cpp gatherUniforms）。
    //   这里复刻同一套：记下每个 program 的 structName 前缀。
    QByteArray m_prefixLin, m_prefixRes, m_prefixArH, m_prefixArV;
    int m_srcW = 0, m_srcH = 0, m_dstW = 0, m_dstH = 0;
    bool m_useAR = false, m_useLin = false;
};

} // namespace rawgl

#pragma once

#include <QString>

class QOpenGLContext;
class QRhi;

// ══ 无设备节点的 headless GL 引导（**仅开发/自检/CI 用**）══════════════════════
//
// 为什么需要它：`QOpenGLContext` 要走 QPA 平台的 GL 集成。本仓库的开发/自检环境
// （沙箱、CI、无 `/dev/dri` 的容器）里：
//   · `QT_QPA_PLATFORM=offscreen` 只实现 **GLX**（要连 X server）→ 没有 GLX 就建不出上下文；
//   · `minimalegl` 需要 EGLConfig 的 pbuffer 支持，surfaceless 平台给不出 → 也失败；
//   · 于是 Qt Quick 静默回退到 **software** 后端，QQuickRhiItem 什么都不画。
//
// 正确做法（不是绕过渲染路径）：像 Qt 自己的 headless 测试那样，用
// **EGL surfaceless（Mesa）+ 软件栅格器 llvmpipe** 自己建一个真正的 GL 上下文，
// 经 `QNativeInterface::QEGLContext::fromNative()` 交给 Qt/QRhi。
// 于是 **RHI 管线、qsb shader、QRhi 资源路径全部是产品那条路**，只是光栅化在 CPU 上。
//
// · 生产路径**完全不需要**它：真机有 GPU，Qt Quick 自己就能建 RHI。
// · 只在显式请求时启用（`ib-test-gpuscale` 套件，或 `PHOTOSET_HEADLESS_GL=1`）。
// · 拿不到 EGL 就返回 nullptr，调用方照常报错/回退，不静默兜底。
namespace headlessgl {

class Context
{
public:
    // 失败返回 nullptr（原因写进 *err）
    static Context *create(QString *err = nullptr);
    ~Context();

    // 交给 QRhi 的 Qt 包装（**可能为空**：QPA 平台不是 EGL 平台时 Qt 拒绝收养，
    //   但 EGL 上下文本身可用 —— 自检的 raw GL 驱动就吃这一口）
    QOpenGLContext *qtContext() const { return m_ctx; }
    bool hasQtContext() const { return m_ctx != nullptr; }
    QString qtAdoptError() const { return m_adoptErr; }
    // 只要 EGL 上下文建成了就算可用（raw GL 驱动只要这个）
    bool isValid() const { return m_eglContext != nullptr; }
    // 给「自检专用的 raw GL 驱动」用（见 gpuscalecheck.cpp）：直接拿到 EGL 句柄自己 makeCurrent
    void *eglDisplay() const { return m_display; }
    void *eglContext() const { return m_eglContext; }
    bool makeCurrent();
    void doneCurrent();
    // 例："llvmpipe (LLVM 23.1.1, 256 bits) / 4.6 (Core Profile) Mesa 26.2.4"（日志/截图说明用）
    QString rendererInfo() const { return m_info; }

private:
    Context() = default;
    QOpenGLContext *m_ctx = nullptr;
    void *m_display = nullptr;
    void *m_eglContext = nullptr;
    QString m_info;
    QString m_adoptErr;
};

// 用导入的上下文建 QRhi（OpenGL 后端）。失败返回 nullptr。
QRhi *createRhi(Context *ctx, QString *err = nullptr);

} // namespace headlessgl

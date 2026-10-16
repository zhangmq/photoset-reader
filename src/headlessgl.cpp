#include "headlessgl.h"

// 只在 Linux/EGL 环境编译（其它平台整体退化为返回 nullptr）
#if defined(Q_OS_LINUX) && __has_include(<EGL/egl.h>) && __has_include(<EGL/eglext.h>)
#  define GPUWIRE_HAS_EGL 1
#endif

#ifdef GPUWIRE_HAS_EGL
#  include <EGL/egl.h>
#  include <EGL/eglext.h>
#  include <GL/gl.h>
#endif

#include <QDebug>
#include <QOpenGLContext>
#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qrhi_platform.h>

#include <cstring>

namespace headlessgl {

#ifdef GPUWIRE_HAS_EGL
namespace {

QString eglErr(const char *what)
{
    return QStringLiteral("EGL %1 失败（0x%2）").arg(QLatin1String(what))
        .arg(eglGetError(), 0, 16);
}

} // namespace
#endif

Context *Context::create(QString *err)
{
#ifdef GPUWIRE_HAS_EGL
    auto fail = [err](const QString &m) {
        if (err) *err = m;
        return static_cast<Context *>(nullptr);
    };

    // ① surfaceless EGL display（Mesa 的 EGL_MESA_platform_surfaceless）
    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (getPlatformDisplay)
        dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY)
        return fail(QStringLiteral("拿不到 EGL display（surfaceless 与默认都失败）"));

    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min))
        return fail(eglErr("eglInitialize"));
    if (!eglBindAPI(EGL_OPENGL_API)) {
        // 桌面 GL 不可用（可能只有 GLES）→ 这里只服务桌面 GL 路径
        return fail(eglErr("eglBindAPI(OpenGL)"));
    }

    // ② 配置（先要 pbuffer 的 RGBA8888 桌面 GL；Mesa surfaceless 一般只给 1 个）
    const EGLint attrsPB[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                              EGL_NONE };
    const EGLint attrsAny[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                EGL_NONE };
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attrsPB, &cfg, 1, &n) || n == 0) {
        cfg = nullptr;
        n = 0;
        if (!eglChooseConfig(dpy, attrsAny, &cfg, 1, &n) || n == 0)
            return fail(eglErr("eglChooseConfig"));
    }

    // ③ 上下文：先要 4.5 core（与 gpubench 的桌面 GL 口径一致），退化为默认
    const EGLint ctxCore[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                               EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                               EGL_NONE };
    EGLContext ectx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxCore);
    if (ectx == EGL_NO_CONTEXT)
        ectx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, nullptr);
    if (ectx == EGL_NO_CONTEXT)
        return fail(eglErr("eglCreateContext"));

    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ectx))
        return fail(eglErr("eglMakeCurrent(surfaceless)"));

    // ④ 试着交给 Qt（QNativeInterface::QEGLContext::fromNative）
    //    ★ 这一步**只在 QPA 平台本身就是 EGL 平台时成功**（Qt 的实现会 dynamic_cast 平台集成，
    //      不是就直接返回 nullptr）。offscreen(仅 GLX)/xcb 上会失败 —— 那就只保留 EGL 上下文，
    //      给自检的 raw GL 驱动用（生产路径不依赖这一步：真机上 Qt Quick 自己建 RHI）。
    QOpenGLContext *qtCtx = QNativeInterface::QEGLContext::fromNative(ectx, dpy);

    auto *self = new Context;
    self->m_ctx = qtCtx;
    self->m_display = dpy;
    self->m_eglContext = ectx;
    if (!qtCtx)
        self->m_adoptErr = QStringLiteral("QEGLContext::fromNative 失败（当前 QPA 平台不是 EGL 平台）");

    // ⑤ 记下 renderer/version（用 eglGetProcAddress 拿 glGetString，避免依赖 GL 链接）
    auto glGetStringFn = reinterpret_cast<const GLubyte *(*)(GLenum)>(eglGetProcAddress("glGetString"));
    if (glGetStringFn) {
        const char *r = reinterpret_cast<const char *>(glGetStringFn(GL_RENDERER));
        const char *v = reinterpret_cast<const char *>(glGetStringFn(GL_VERSION));
        self->m_info = QStringLiteral("%1 / %2").arg(QLatin1String(r ? r : "?"), QLatin1String(v ? v : "?"));
    }

    // ⑥ 释放本线程的 EGL 绑定：渲染线程（Qt Quick）后面要自己 makeCurrent
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return self;
#else
    if (err)
        *err = QStringLiteral("本平台没有 EGL（headless GL 引导不可用）");
    return nullptr;
#endif
}

bool Context::makeCurrent()
{
#ifdef GPUWIRE_HAS_EGL
    if (!m_display || !m_eglContext)
        return false;
    return eglMakeCurrent(static_cast<EGLDisplay>(m_display), EGL_NO_SURFACE, EGL_NO_SURFACE,
                          static_cast<EGLContext>(m_eglContext));
#else
    return false;
#endif
}

void Context::doneCurrent()
{
#ifdef GPUWIRE_HAS_EGL
    if (m_display)
        eglMakeCurrent(static_cast<EGLDisplay>(m_display), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#endif
}

Context::~Context()
{
#ifdef GPUWIRE_HAS_EGL
    delete m_ctx;                       // 先销毁 Qt 包装（可能为空）
    m_ctx = nullptr;
    if (m_display && m_eglContext) {
        EGLDisplay dpy = static_cast<EGLDisplay>(m_display);
        eglDestroyContext(dpy, static_cast<EGLContext>(m_eglContext));
        // 不调 eglTerminate：display 可能被 Qt/其它组件共用（dev/CI 进程随即退出）
    }
#endif
}

QRhi *createRhi(Context *ctx, QString *err)
{
    if (!ctx || !ctx->isValid()) {
        if (err) *err = QStringLiteral("headless GL 上下文不可用");
        return nullptr;
    }
    if (!ctx->hasQtContext()) {
        if (err) *err = ctx->qtAdoptError();
        return nullptr;
    }
    QRhiGles2InitParams params;
    params.format = QSurfaceFormat::defaultFormat();
    params.fallbackSurface = nullptr;      // 我们导入了 surfaceless 上下文，不需要 fallback surface
    QRhiGles2NativeHandles handles;
    handles.context = ctx->qtContext();
    QRhi *rhi = QRhi::create(QRhi::OpenGLES2, &params, {}, &handles);
    if (!rhi && err)
        *err = QStringLiteral("QRhi::create(OpenGL, 导入上下文) 失败");
    return rhi;
}

} // namespace headlessgl

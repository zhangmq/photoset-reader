#include "rawgldriver.h"

#include "gpuscale.h"
#include "headlessgl.h"

#include <QDebug>
#include <QFile>
#include <QtGui/rhi/qshader.h>

#if defined(GPUWIRE_HAVE_EGL) && __has_include(<GL/gl.h>)
#  define GPUWIRE_HAVE_RAWGL 1
#  define GL_GLEXT_PROTOTYPES 1
#  include <GL/gl.h>
#  include <GL/glext.h>
#endif

namespace rawgl {

#ifdef GPUWIRE_HAVE_RAWGL
namespace {

GLuint compile(GLenum type, const QByteArray &src, const char *what)
{
    const GLuint s = glCreateShader(type);
    const char *p = src.constData();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        qWarning().noquote() << QStringLiteral("[rawgl] 编译 %1 失败：%2").arg(QLatin1String(what), QLatin1String(log));
        return 0;
    }
    return s;
}

QShader qsbShader(const QString &path, const char *what)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << QStringLiteral("[rawgl] 打不开 %1").arg(path);
        return QShader();
    }
    const QShader sh = QShader::fromSerialized(f.readAll());
    if (!sh.isValid())
        qWarning().noquote() << QStringLiteral("[rawgl] %1 反序列化失败").arg(path);
    Q_UNUSED(what);
    return sh;
}

// 从 qsb（**产品用的同一份烘焙产物**）里取 GLSL 源（取**最高**可用版本，与 QRhi 的
// glslVersionsToTry 同序；qsb 在 GLSL 目标上把 UBO 展平成普通 struct uniform，
//  这也正是 QRhi 的 GL 后端所预期的形式，见 qrhigles2.cpp gatherUniforms）
QByteArray glslFromQsb(const QShader &sh, const char *what)
{
    if (!sh.isValid())
        return QByteArray();
    // 与 QRhi 的 glslVersionsToTry 同序：挑**最高**的可用变体（UBO 块只在较高版本里是真正的块）
    QByteArray picked;
    int pickedVer = 0;
    if (qEnvironmentVariableIsSet("PHOTOSET_RAWGL_DEBUG")) {
        static const int vers[] = { 460, 450, 440, 430, 420, 410, 400, 330, 150, 140, 130 };
        for (int v : vers) {
            const QByteArray t = sh.shader(QShaderKey(QShader::GlslShader, QShaderVersion(v))).shader();
            if (t.isEmpty())
                continue;
            const bool block = t.contains("uniform buf\n{") || t.contains("uniform buf {")
                               || t.contains("layout(std140) uniform buf");
            qInfo().noquote() << QStringLiteral("[rawgl] %1: GLSL %2 可用  块写法=%3")
                                     .arg(QLatin1String(what)).arg(v)
                                     .arg(block ? QStringLiteral("真 UBO 块") : QStringLiteral("展平成 uniform 结构"));
        }
    }
    for (int v : { 460, 450, 440, 430, 420, 410, 400, 330, 150, 140, 130 }) {
        const QByteArray t = sh.shader(QShaderKey(QShader::GlslShader, QShaderVersion(v))).shader();
        if (!t.isEmpty()) { picked = t; pickedVer = v; break; }
    }
    const QShaderCode code{ picked, QByteArrayLiteral("main") };
    if (code.shader().isEmpty()) {
        qWarning().noquote() << QStringLiteral("[rawgl] %1 里没有任何 GLSL 变体（qsb 的 -g 列表？）").arg(what);
        return QByteArray();
    }
    if (qEnvironmentVariableIsSet("PHOTOSET_RAWGL_DEBUG"))
        qInfo().noquote() << QStringLiteral("[rawgl] %1 → 用 GLSL %2").arg(QLatin1String(what)).arg(pickedVer);
    if (qEnvironmentVariableIsSet("PHOTOSET_RAWGL_DEBUG")) {
        const QByteArray src = code.shader();
        qInfo().noquote() << QStringLiteral("[rawgl] %1 的 GLSL 330（前 1200 字节）:\n%2")
                                 .arg(QLatin1String(what), QString::fromUtf8(src.left(1200)));
    }
    return code.shader();
}

GLuint link(const QString &vsPath, const QString &fsPath, const char *what, QByteArray *prefixOut)
{
    const QShader vsh = qsbShader(vsPath, "vertex");
    const QShader fsh = qsbShader(fsPath, what);
    const QByteArray vs = glslFromQsb(vsh, "vertex");
    const QByteArray fs = glslFromQsb(fsh, what);
    if (vs.isEmpty() || fs.isEmpty())
        return 0;
    const GLuint v = compile(GL_VERTEX_SHADER, vs, "vertex");
    const GLuint s = compile(GL_FRAGMENT_SHADER, fs, what);
    if (!v || !s)
        return 0;
    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, s);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {0};
        glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
        qWarning().noquote() << QStringLiteral("[rawgl] 链接 %1 失败：%2").arg(QLatin1String(what), QLatin1String(log));
        return 0;
    }
    // 真 UBO 块（若该 GLSL 变体保留了块）→ binding 0
    const GLuint bi = glGetUniformBlockIndex(p, "buf");
    if (bi != GL_INVALID_INDEX)
        glUniformBlockBinding(p, bi, 0);
    // 展平形式：记下 struct 实例名（QRhi 的 GL 后端同款做法）
    if (prefixOut) {
        prefixOut->clear();
        const QShaderDescription desc = fsh.description();
        for (const QShaderDescription::UniformBlock &ub : desc.uniformBlocks()) {
            if (ub.blockName == QByteArrayLiteral("buf")) {
                *prefixOut = ub.structName + '.';
                break;
            }
        }
    }
    // ⚠ 采样器 uniform 必须**在 glUseProgram 之后**设（早期实验工具记下的坑：否则 GL_INVALID_OPERATION）
    return p;
}

GLuint makeTex(int w, int h, GLint internal, GLenum fmt, GLenum type)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, type, nullptr);
    return t;
}

GLuint makeFbo(GLuint tex, GLuint minTex = 0, GLuint maxTex = 0)
{
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (minTex)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, minTex, 0);
    if (maxTex)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, maxTex, 0);
    if (minTex) {
        const GLenum bufs[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
        glDrawBuffers(3, bufs);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        qWarning() << "[rawgl] FBO 不完整";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return fb;
}

} // namespace
#endif // GPUWIRE_HAVE_RAWGL

Driver *Driver::create(QString *err)
{
#ifdef GPUWIRE_HAVE_RAWGL
    QString e;
    headlessgl::Context *ctx = headlessgl::Context::create(&e);
    if (!ctx) {
        if (err) *err = e;
        return nullptr;
    }
    if (!ctx->makeCurrent()) {
        if (err) *err = QStringLiteral("EGL makeCurrent 失败");
        delete ctx;
        return nullptr;
    }
    auto *d = new Driver;
    d->m_ctx = ctx;
    d->m_info = ctx->rendererInfo();
    return d;
#else
    if (err)
        *err = QStringLiteral("本平台没有 EGL/GL（raw GL 自检驱动不可用）");
    return nullptr;
#endif
}

void Driver::dumpProgramInfo()
{
#ifdef GPUWIRE_HAVE_RAWGL
    struct P { const char *name; GLuint p; };
    const P ps[4] = { { "linearize", m_progLin }, { "resample", m_progRes },
                      { "ar-H", m_progArH }, { "ar-V", m_progArV } };
    for (const P &x : ps) {
        if (!x.p)
            continue;
        const GLuint bi = glGetUniformBlockIndex(x.p, "buf");
        GLint blockSize = -1;
        if (bi != GL_INVALID_INDEX)
            glGetActiveUniformBlockiv(x.p, bi, GL_UNIFORM_BLOCK_DATA_SIZE, &blockSize);
        const GLint lIn = glGetUniformLocation(x.p, "uIn");
        const GLint lMin = glGetUniformLocation(x.p, "uMin");
        const GLint lMax = glGetUniformLocation(x.p, "uMax");
        qInfo().noquote() << QStringLiteral("[rawgl] %1: blockIdx=%2 blockSize=%3 uIn=%4 uMin=%5 uMax=%6")
                                 .arg(QLatin1String(x.name)).arg(bi).arg(blockSize)
                                 .arg(lIn).arg(lMin).arg(lMax);
    }
#endif
}

Driver::~Driver()
{
#ifdef GPUWIRE_HAVE_RAWGL
    if (m_ctx) {
        static_cast<headlessgl::Context *>(m_ctx)->makeCurrent();
        release();
        static_cast<headlessgl::Context *>(m_ctx)->doneCurrent();
        delete static_cast<headlessgl::Context *>(m_ctx);
    }
#endif
}

#ifdef GPUWIRE_HAVE_RAWGL
void Driver::release()
{
    if (m_progLin) glDeleteProgram(m_progLin);
    if (m_progRes) glDeleteProgram(m_progRes);
    if (m_progArH) glDeleteProgram(m_progArH);
    if (m_progArV) glDeleteProgram(m_progArV);
    m_progLin = m_progRes = m_progArH = m_progArV = 0;
    GLuint texs[8] = { m_srcTex, m_linTex, m_hTex, m_hMin, m_hMax, m_outTex, m_boundsH, m_boundsV };
    for (GLuint t : texs)
        if (t) glDeleteTextures(1, &t);
    m_srcTex = m_linTex = m_hTex = m_hMin = m_hMax = m_outTex = m_boundsH = m_boundsV = 0;
    GLuint fbs[4] = { m_fboLin, m_fboH, m_fboHAr, m_fboOut };
    for (GLuint f : fbs)
        if (f) glDeleteFramebuffers(1, &f);
    m_fboLin = m_fboH = m_fboHAr = m_fboOut = 0;
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_ubo) glDeleteBuffers(1, &m_ubo);
    m_vbo = m_vao = m_ubo = 0;
    m_srcW = m_srcH = m_dstW = m_dstH = 0;
}

bool Driver::createProgramsAndBuffers(QString *err)
{
    m_progLin = link(QStringLiteral(":/gpuscale.vert.qsb"),
                     QStringLiteral(":/gpuscale_linearize.frag.qsb"), "linearize", &m_prefixLin);
    m_progRes = link(QStringLiteral(":/gpuscale.vert.qsb"),
                     QStringLiteral(":/gpuscale_resample.frag.qsb"), "resample", &m_prefixRes);
    m_progArH = link(QStringLiteral(":/gpuscale.vert.qsb"),
                     QStringLiteral(":/gpuscale_resample_ar_h.frag.qsb"), "resample-ar-H", &m_prefixArH);
    m_progArV = link(QStringLiteral(":/gpuscale.vert.qsb"),
                     QStringLiteral(":/gpuscale_resample_ar_v.frag.qsb"), "resample-ar-V", &m_prefixArV);
    if (!m_progLin || !m_progRes || !m_progArH || !m_progArV) {
        if (err) *err = QStringLiteral("shader 编译/链接失败");
        return false;
    }
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };   // （与早期实验工具同序）
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glGenBuffers(1, &m_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, m_ubo);
    glBufferData(GL_UNIFORM_BUFFER, 64, nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_ubo);
    return true;
}

bool Driver::ensure(const gpuscale::Plan &plan, QString *err)
{
    const int srcW = plan.src.width();
    const int srcH = plan.src.height();
    const int dstW = plan.dst.width();
    const int dstH = plan.dst.height();
    const bool useAR = plan.useAR;
    const bool useLinearize = plan.useLinearize;
    if (!m_progRes) {
        if (!createProgramsAndBuffers(err))
            return false;
    }

    if (srcW == m_srcW && srcH == m_srcH && dstW == m_dstW && dstH == m_dstH
        && useAR == m_useAR && useLinearize == m_useLin && m_outTex)
        return true;

    release();
    // release() 把 program 也删了 → 重新建（简单、正确；自检不在乎这点开销）
    if (!m_progRes) {
        if (!createProgramsAndBuffers(err))
            return false;
    }

    m_srcW = srcW;
    m_srcH = srcH;
    m_dstW = dstW;
    m_dstH = dstH;
    m_useAR = useAR;
    m_useLin = useLinearize;

    m_srcTex = makeTex(srcW, srcH, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    m_linTex = makeTex(srcW, srcH, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    m_hTex = makeTex(dstW, srcH, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    m_fboLin = makeFbo(m_linTex);
    if (useAR) {
        m_hMin = makeTex(dstW, srcH, GL_RGBA16F, GL_RGBA, GL_FLOAT);
        m_hMax = makeTex(dstW, srcH, GL_RGBA16F, GL_RGBA, GL_FLOAT);
        m_fboHAr = makeFbo(m_hTex, m_hMin, m_hMax);
    } else {
        m_fboH = makeFbo(m_hTex);
    }
    m_outTex = makeTex(dstW, dstH, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    m_fboOut = makeFbo(m_outTex);

    return true;
}
#endif // GPUWIRE_HAVE_RAWGL

bool Driver::render(const QImage &src32, const gpuscale::Plan &plan, QImage *out, QString *err)
{
#ifdef GPUWIRE_HAVE_RAWGL
    if (!m_ctx) {
        if (err) *err = QStringLiteral("驱动未初始化");
        return false;
    }
    if (!plan.valid()) {
        if (err) *err = QStringLiteral("无效的 Plan");
        return false;
    }
    if (src32.size() != plan.src) {
        if (err) *err = QStringLiteral("源尺寸与 Plan 不符");
        return false;
    }
    if (!ensure(plan, err))
        return false;

    // 抽头边界（CPU 用 double 算好；与产品 Pipeline 同一份数据）
    //   ★ 每次 render 都按当前 plan 重新生成 —— ensure() 的早退只看尺寸/AR/useLin，
    //     而边界**依赖滤波器**（box 支撑 0.5 / Mitchell 2.0 / Lanczos lobes），不能复用。
    {
        const QByteArray hb = gpuscale::tapBoundsBytes(plan.src.width(), plan.full.width(), plan.offX,
                                                       plan.dst.width(), plan.params, !plan.useLinearize);
        const QByteArray vb = gpuscale::tapBoundsBytes(plan.src.height(), plan.full.height(), plan.offY,
                                                       plan.dst.height(), plan.params, !plan.useLinearize);
        if (hb.isEmpty() || vb.isEmpty()) {
            if (err) *err = QStringLiteral("抽头边界数据生成失败");
            return false;
        }
        if (m_boundsH) { glDeleteTextures(1, &m_boundsH); m_boundsH = 0; }
        if (m_boundsV) { glDeleteTextures(1, &m_boundsV); m_boundsV = 0; }
        m_boundsH = makeTex(plan.dst.width(), 1, GL_RGBA32F, GL_RGBA, GL_FLOAT);
        m_boundsV = makeTex(plan.dst.height(), 1, GL_RGBA32F, GL_RGBA, GL_FLOAT);
        glBindTexture(GL_TEXTURE_2D, m_boundsH);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, plan.dst.width(), 1, GL_RGBA, GL_FLOAT, hb.constData());
        glBindTexture(GL_TEXTURE_2D, m_boundsV);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, plan.dst.height(), 1, GL_RGBA, GL_FLOAT, vb.constData());
    }

    const QImage src = src32.convertToFormat(QImage::Format_RGBA8888);
    glBindTexture(GL_TEXTURE_2D, m_srcTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src.width(), src.height(), GL_RGBA, GL_UNSIGNED_BYTE,
                    src.constBits());

    // ★ flipY：GL 上 QQuickRhiItem 合成时会镜像 UV ⇒ 垂直趟要翻索引（见 gpuscale.h）
    const bool flipY = true;

    const bool dbg = qEnvironmentVariableIsSet("PHOTOSET_RAWGL_DEBUG");
    auto pass = [&](GLuint prog, GLuint fbo, GLuint inTex, GLuint boundsTex, int outW, int outH,
                    const QByteArray &prefix,
                    const gpuscale::PassUniforms &u, GLuint minTex = 0, GLuint maxTex = 0) {
        float packed[16];
        u.pack(packed);
        if (dbg)
            qInfo().noquote() << QStringLiteral("[rawgl] pass prog=%1 fbo=%2 in=%3 %4x%5 uniforms=%6")
                                     .arg(prog).arg(fbo).arg(inTex).arg(outW).arg(outH)
                                     .arg(QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(packed), sizeof(packed)).toHex(' ')));
        glBindBuffer(GL_UNIFORM_BUFFER, m_ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, 64, packed);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, outW, outH);
        glUseProgram(prog);
        // ★ glUniform* 必须在 glUseProgram **之后**；展平形式按 `structName.pN` 逐成员设
        //   （QRhi 的 GL 后端同款，见 qrhigles2.cpp gatherUniforms）
        if (!prefix.isEmpty()) {
            const char *members[4] = { "p0", "p1", "p2", "p3" };
            for (int i = 0; i < 4; ++i) {
                const QByteArray name = prefix + members[i];
                const GLint loc = glGetUniformLocation(prog, name.constData());
                if (loc >= 0)
                    glUniform4fv(loc, 1, packed + 4 * i);
            }
        } else {
            glBindBuffer(GL_UNIFORM_BUFFER, m_ubo);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, 64, packed);
        }
        // ★ glUniform* 必须在 glUseProgram **之后**（早期实验工具记下的坑）
        //   **纹理单元 = 绑定号**（与 QRhi 的 GL 后端一致）：1=uIn、2=uBounds、3=uMin、4=uMax
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, inTex);
        glUniform1i(glGetUniformLocation(prog, "uIn"), 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, boundsTex);
        glUniform1i(glGetUniformLocation(prog, "uBounds"), 2);
        if (minTex) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, minTex);
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, maxTex);
            glUniform1i(glGetUniformLocation(prog, "uMin"), 3);
            glUniform1i(glGetUniformLocation(prog, "uMax"), 4);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(m_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };

    if (plan.useLinearize)
        pass(m_progLin, m_fboLin, m_srcTex, 0, plan.src.width(), plan.src.height(), m_prefixLin,
             gpuscale::passUniformsFor(plan, 0, flipY));
    // ★ nearest 通路没有线性化趟 → H 趟直接吃源纹理；抽头边界由 CPU 给（m_boundsH/V）
    pass(plan.useAR ? m_progArH : m_progRes, plan.useAR ? m_fboHAr : m_fboH,
         plan.useLinearize ? m_linTex : m_srcTex, m_boundsH,
         plan.dst.width(), plan.src.height(),
         plan.useAR ? m_prefixArH : m_prefixRes,
         gpuscale::passUniformsFor(plan, 1, flipY));
    pass(plan.useAR ? m_progArV : m_progRes, m_fboOut, m_hTex, m_boundsV,
         plan.dst.width(), plan.dst.height(),
         plan.useAR ? m_prefixArV : m_prefixRes,
         gpuscale::passUniformsFor(plan, 2, flipY), m_hMin, m_hMax);
    glFinish();

    QImage img(plan.dst, QImage::Format_RGBA8888);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fboOut);
    glReadPixels(0, 0, plan.dst.width(), plan.dst.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    glFinish();
    // GL 的 framebuffer 行 0 在下 → 翻一次得到自上而下的 QImage（与 QRhi 文档的口径一致）
    *out = img.mirrored(false, true);

    if (const GLenum e = glGetError(); e != GL_NO_ERROR) {
        if (err) *err = QStringLiteral("GL 错误 0x%1").arg(e, 0, 16);
        return false;
    }
    return true;
#else
    Q_UNUSED(src32);
    Q_UNUSED(plan);
    Q_UNUSED(out);
    if (err) *err = QStringLiteral("本平台没有 EGL/GL");
    return false;
#endif
}

} // namespace rawgl

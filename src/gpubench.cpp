#include "gpubench.h"

#include "imageprovider.h"   // 复用 prepareReader（shrink-on-load 口径与生产一致）
#include "scaler.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QImageWriter>
#include <QImage>
#include <QImageReader>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QSurfaceFormat>
#include <QDebug>

#include <algorithm>
#include <vector>

namespace {

// ── 着色器 ────────────────────────────────────────────────────
QByteArray header(bool es)
{
    return es ? QByteArrayLiteral("#version 300 es\nprecision highp float;\nprecision highp int;\n")
              : QByteArrayLiteral("#version 330 core\n");
}

const char *kVertBody = R"(
layout(location = 0) in vec2 aPos;
out vec2 vUv;
void main() { vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }
)";

// ① 线性化：sRGB8 → 线性 float
const char *kFragLinBody = R"(
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uIn;
vec3 s2l(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
void main() { fragColor = vec4(s2l(texture(uIn, vUv).rgb), 1.0); }
)";

// ②③ 分离式 Lanczos（核按缩小比例加宽，与 CPU 的 buildTaps 同规则）
const char *kFragLanczosBody = R"(
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uIn;
uniform int   uFilter;    // 0 = Lanczos3，1 = Mitchell
uniform int   uOutN;      // 本方向输出像素数
uniform int   uInN;       // 本方向输入像素数
uniform int   uAxis;      // 0 = 水平，1 = 垂直
uniform float uFscale;
uniform float uRadius;
uniform int   uEncode;    // 1 = 输出前做 linear→sRGB
vec3 l2s(vec3 c) { c = clamp(c, 0.0, 1.0); return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
uniform int   uLobes;
uniform float uMB;
uniform float uMC;
float sinc_(float x) { return x == 0.0 ? 1.0 : sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x); }
float lN(float x) { float a = float(uLobes); return (x <= -a || x >= a) ? 0.0 : sinc_(x) * sinc_(x / a); }
float mitchellBC(float x) {
    float a = abs(x);
    if (a < 1.0) return ((12.0 - 9.0*uMB - 6.0*uMC)*a*a*a + (-18.0 + 12.0*uMB + 6.0*uMC)*a*a + (6.0 - 2.0*uMB)) / 6.0;
    if (a < 2.0) return ((-uMB - 6.0*uMC)*a*a*a + (6.0*uMB + 30.0*uMC)*a*a + (-12.0*uMB - 48.0*uMC)*a + (8.0*uMB + 24.0*uMC)) / 6.0;
    return 0.0;
}
float wt(float x) { return uFilter == 2 ? mitchellBC(x) : (uFilter == 1 ? 1.0 : lN(x)); }
void main() {
    float i = (uAxis == 0 ? gl_FragCoord.x : gl_FragCoord.y) - 0.5;
    float scale = float(uInN) / float(uOutN);
    float center = (i + 0.5) * scale - 0.5;
    int lo = int(floor(center - uRadius + 0.5));
    int hi = int(ceil(center + uRadius - 0.5));
    lo = max(lo, 0);
    hi = min(hi, uInN - 1);
    vec3 acc = vec3(0.0);
    float sum = 0.0;
    for (int k = lo; k <= hi; ++k) {
        float w = wt((float(k) - center) / uFscale);
        vec2 uv = vUv;
        if (uAxis == 0) uv.x = (float(k) + 0.5) / float(uInN);
        else            uv.y = (float(k) + 0.5) / float(uInN);
        acc += texture(uIn, uv).rgb * w;
        sum += w;
    }
    vec3 c = acc / max(sum, 1e-6);
    if (uEncode == 1) c = l2s(c);
    fragColor = vec4(c, 1.0);
}
)";


// ── anti-ringing（best 档）：复刻 CPU 的两趟钳位 ──
//   H 趟：输出 color + 源邻域 min/max（三目标 MRT）
const char *kFragArHBody = R"(
in vec2 vUv;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 outMin;
layout(location = 2) out vec4 outMax;
uniform sampler2D uIn;
uniform int   uFilter;
uniform int   uOutN;
uniform int   uInN;
uniform int   uAxis;
uniform float uFscale;
uniform float uRadius;
uniform int   uLobes;
uniform float uMB;
uniform float uMC;
float sinc_(float x) { return x == 0.0 ? 1.0 : sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x); }
float lN(float x) { float a = float(uLobes); return (x <= -a || x >= a) ? 0.0 : sinc_(x) * sinc_(x / a); }
float mitchellBC(float x) {
    float a = abs(x);
    if (a < 1.0) return ((12.0 - 9.0*uMB - 6.0*uMC)*a*a*a + (-18.0 + 12.0*uMB + 6.0*uMC)*a*a + (6.0 - 2.0*uMB)) / 6.0;
    if (a < 2.0) return ((-uMB - 6.0*uMC)*a*a*a + (6.0*uMB + 30.0*uMC)*a*a + (-12.0*uMB - 48.0*uMC)*a + (8.0*uMB + 24.0*uMC)) / 6.0;
    return 0.0;
}
float wt(float x) { return uFilter == 2 ? mitchellBC(x) : (uFilter == 1 ? 1.0 : lN(x)); }
void main() {
    float i = (uAxis == 0 ? gl_FragCoord.x : gl_FragCoord.y) - 0.5;
    float scale = float(uInN) / float(uOutN);
    float center = (i + 0.5) * scale - 0.5;
    int k0 = max(int(floor(center - uRadius + 0.5)), 0);
    int k1 = min(int(ceil(center + uRadius - 0.5)), uInN - 1);
    vec3 acc = vec3(0.0); float sum = 0.0;
    vec3 vmin = vec3(1e30); vec3 vmax = vec3(-1e30);
    for (int k = k0; k <= k1; ++k) {
        float w = wt((float(k) - center) / uFscale);
        vec2 uv = vUv;
        if (uAxis == 0) uv.x = (float(k) + 0.5) / float(uInN);
        else            uv.y = (float(k) + 0.5) / float(uInN);
        vec3 v = texture(uIn, uv).rgb;
        acc += v * w; sum += w;
        vmin = min(vmin, v); vmax = max(vmax, v);
    }
    fragColor = vec4(acc / max(sum, 1e-6), 1.0);
    outMin = vec4(vmin, 1.0);
    outMax = vec4(vmax, 1.0);
}
)";

//   V 趟：用 H 趟传下来的 min/max 夹住结果，再回 sRGB
const char *kFragArVBody = R"(
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uIn;
uniform sampler2D uMin;
uniform sampler2D uMax;
uniform int   uFilter;
uniform int   uOutN;
uniform int   uInN;
uniform int   uAxis;
uniform float uFscale;
uniform float uRadius;
uniform int   uEncode;
vec3 l2s(vec3 c) { c = clamp(c, 0.0, 1.0); return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
uniform int   uLobes;
uniform float uMB;
uniform float uMC;
float sinc_(float x) { return x == 0.0 ? 1.0 : sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x); }
float lN(float x) { float a = float(uLobes); return (x <= -a || x >= a) ? 0.0 : sinc_(x) * sinc_(x / a); }
float mitchellBC(float x) {
    float a = abs(x);
    if (a < 1.0) return ((12.0 - 9.0*uMB - 6.0*uMC)*a*a*a + (-18.0 + 12.0*uMB + 6.0*uMC)*a*a + (6.0 - 2.0*uMB)) / 6.0;
    if (a < 2.0) return ((-uMB - 6.0*uMC)*a*a*a + (6.0*uMB + 30.0*uMC)*a*a + (-12.0*uMB - 48.0*uMC)*a + (8.0*uMB + 24.0*uMC)) / 6.0;
    return 0.0;
}
float wt(float x) { return uFilter == 2 ? mitchellBC(x) : (uFilter == 1 ? 1.0 : lN(x)); }
void main() {
    float i = (uAxis == 0 ? gl_FragCoord.x : gl_FragCoord.y) - 0.5;
    float scale = float(uInN) / float(uOutN);
    float center = (i + 0.5) * scale - 0.5;
    int k0 = max(int(floor(center - uRadius + 0.5)), 0);
    int k1 = min(int(ceil(center + uRadius - 0.5)), uInN - 1);
    vec3 acc = vec3(0.0); float sum = 0.0;
    vec3 vmin = vec3(1e30); vec3 vmax = vec3(-1e30);
    for (int k = k0; k <= k1; ++k) {
        float w = wt((float(k) - center) / uFscale);
        vec2 uv = vUv;
        if (uAxis == 0) uv.x = (float(k) + 0.5) / float(uInN);
        else            uv.y = (float(k) + 0.5) / float(uInN);
        acc += texture(uIn, uv).rgb * w; sum += w;
        vmin = min(vmin, texture(uMin, uv).rgb);
        vmax = max(vmax, texture(uMax, uv).rgb);
    }
    vec3 c = clamp(acc / max(sum, 1e-6), vmin, vmax);
    if (uEncode == 1) c = l2s(c);
    fragColor = vec4(c, 1.0);
}
)";

GLuint compile(QOpenGLExtraFunctions *f, GLenum type, const QByteArray &src, const char *what)
{
    const GLuint s = f->glCreateShader(type);
    const char *p = src.constData();
    f->glShaderSource(s, 1, &p, nullptr);
    f->glCompileShader(s);
    GLint ok = 0;
    f->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        f->glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        qWarning().noquote() << QStringLiteral("[gpu] 编译 %1 失败: %2").arg(QLatin1String(what), QLatin1String(log));
        return 0;
    }
    return s;
}

GLuint link(QOpenGLExtraFunctions *f, bool es, const char *fragBody, const char *what)
{
    const QByteArray vs = header(es) + QByteArray(kVertBody);
    const QByteArray fs = header(es) + QByteArray(fragBody);
    const GLuint v = compile(f, GL_VERTEX_SHADER, vs, "vertex");
    const GLuint s = compile(f, GL_FRAGMENT_SHADER, fs, what);
    if (!v || !s) return 0;
    const GLuint p = f->glCreateProgram();
    f->glAttachShader(p, v);
    f->glAttachShader(p, s);
    f->glLinkProgram(p);
    GLint ok = 0;
    f->glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        f->glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
        qWarning().noquote() << QStringLiteral("[gpu] 链接 %1 失败: %2").arg(QLatin1String(what), QLatin1String(log));
        return 0;
    }
    return p;
}

GLuint makeTexture(QOpenGLExtraFunctions *f, int w, int h, GLint internalFmt, GLenum fmt, GLenum type)
{
    GLuint t = 0;
    f->glGenTextures(1, &t);
    f->glBindTexture(GL_TEXTURE_2D, t);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, fmt, type, nullptr);
    return t;
}

GLuint makeFbo(QOpenGLExtraFunctions *f, GLuint tex)
{
    GLuint fb = 0;
    f->glGenFramebuffers(1, &fb);
    f->glBindFramebuffer(GL_FRAMEBUFFER, fb);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum st = f->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        qWarning().noquote() << QStringLiteral("[gpu] FBO 不完整: 0x%1").arg(st, 0, 16);
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return fb;
}

double median(std::vector<double> v)
{
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

int runGpuScaleBench(const QStringList &imagePaths, const QSize &box, int repeats,
                     const scaler::Params &params)
{
    if (imagePaths.isEmpty()) { qWarning() << "[gpu] 没有素材"; return 2; }
    repeats = qMax(1, repeats);
    const scaler::Quality sq = params.quality;
    const scaler::Filter filter = params.filter;
    const int lobes = qBound(2, params.lobes, 4);

    // ── ① 离屏 GL 上下文（显式要桌面 OpenGL，不要 GLES）──
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(4, 6);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext ctx;
    ctx.setFormat(fmt);
    if (!ctx.create()) { qWarning() << "[gpu] 创建 GL 上下文失败"; return 3; }
    QOffscreenSurface surf;
    surf.setFormat(ctx.format());
    surf.create();
    if (!surf.isValid()) { qWarning() << "[gpu] 离屏 surface 失败"; return 3; }
    if (!ctx.makeCurrent(&surf)) { qWarning() << "[gpu] makeCurrent 失败"; return 3; }

    QOpenGLExtraFunctions *f = ctx.extraFunctions();
    const bool es = ctx.isOpenGLES();
    qInfo().noquote() << QStringLiteral("[gpu] 后端 = %1  GLES=%2  %3")
                             .arg(QLatin1String(reinterpret_cast<const char *>(f->glGetString(GL_RENDERER))),
                                  es ? QStringLiteral("是") : QStringLiteral("否"),
                                  QLatin1String(reinterpret_cast<const char *>(f->glGetString(GL_VERSION))));

    const GLuint progLin = link(f, es, kFragLinBody, "linearize");
    const GLuint progLcz = link(f, es, kFragLanczosBody, "lanczos");
    const GLuint progArH = link(f, es, kFragArHBody, "lanczos-ar-H");
    const GLuint progArV = link(f, es, kFragArVBody, "lanczos-ar-V");
    if (!progLin || !progLcz || !progArH || !progArV) return 4;

    // 全屏四边形
    GLuint vao = 0, vbo = 0;
    f->glGenVertexArrays(1, &vao);
    f->glBindVertexArray(vao);
    const float quad[8] = { -1, -1,  1, -1, -1, 1,  1, 1 };
    f->glGenBuffers(1, &vbo);
    f->glBindBuffer(GL_ARRAY_BUFFER, vbo);
    f->glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    struct Row {
        QString name;
        QSize src, dst;
        double upload = 0, lin = 0, h = 0, v = 0, read = 0, total = 0, cpu = 0;
        double meanAbs = -1, pctGt2 = -1;
        QString orient;
        QString grid;
    };
    std::vector<Row> rows;

    for (const QString &path : imagePaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = file.readAll();
        QBuffer buf;
        buf.setData(bytes);
        buf.open(QIODevice::ReadOnly);
        QImageReader reader(&buf);
        reader.setAutoTransform(true);
        // 与生产同口径的 shrink-on-load
        ImageProvider::prepareReader(reader, box, /*shrinkOnLoad=*/true);
        QImage src = reader.read();
        if (src.isNull()) continue;
        const QSize target = src.size().scaled(box, Qt::KeepAspectRatio);
        if (!target.isValid() || target.isEmpty()) continue;

        // ★ 上传前统一成 RGB32：① `GL_BGRA` 只对 0xffRRGGBB 的内存布局成立；
        //   ② CPU 侧的 toLinear 内部也做同样的 convertToFormat，这样两边同口径
        const QImage::Format srcFmt = src.format();
        src = src.convertToFormat(QImage::Format_RGB32);

        // CPU 参考（同一张输入、同一档位）
        std::vector<double> cpuT;
        QImage cpuOut;
        for (int r = 0; r < repeats; ++r) {
            QElapsedTimer t; t.start();
            cpuOut = scaler::resize(src, box, params);
            cpuT.push_back(double(t.nsecsElapsed())/1e6);
        }

        // GPU：纹理 / FBO 按本张尺寸建一次
        const int sw = src.width(), sh = src.height();
        const int tw = target.width(), th = target.height();
        const GLuint texIn = makeTexture(f, sw, sh, GL_RGBA8, es ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE);
        const GLuint texLin = makeTexture(f, sw, sh, GL_RGBA16F, GL_RGBA, GL_FLOAT);
        const GLuint texH   = makeTexture(f, tw, sh, GL_RGBA16F, GL_RGBA, GL_FLOAT);
        const GLuint texOut = makeTexture(f, tw, th, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        const bool useAr = (sq == scaler::Quality::Best);
        const GLuint texHMin = useAr ? makeTexture(f, tw, sh, GL_RGBA16F, GL_RGBA, GL_FLOAT) : 0;
        const GLuint texHMax = useAr ? makeTexture(f, tw, sh, GL_RGBA16F, GL_RGBA, GL_FLOAT) : 0;
        const GLuint fboLin = makeFbo(f, texLin);
        GLuint fboH = 0;
        if (useAr) {   // 3 个颜色附件：color + min + max
            f->glGenFramebuffers(1, &fboH);
            f->glBindFramebuffer(GL_FRAMEBUFFER, fboH);
            f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texH, 0);
            f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, texHMin, 0);
            f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, texHMax, 0);
            if (f->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                qWarning() << "[gpu] AR 的 FBO 不完整";
            f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            fboH = makeFbo(f, texH);
        }
        const GLuint fboOut = makeFbo(f, texOut);

        // 上传一次（后面重复跑只重复"缩放"）
        std::vector<double> upT;
        for (int r = 0; r < repeats; ++r) {
            QElapsedTimer t; t.start();
            f->glBindTexture(GL_TEXTURE_2D, texIn);
            f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh,
                               es ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE, src.constBits());
            f->glFinish();
            if (const GLenum e = f->glGetError(); e != GL_NO_ERROR)
                qWarning().noquote() << QStringLiteral("[gpu] 上传后 GL 错误 0x%1").arg(e, 0, 16);
            upT.push_back(double(t.nsecsElapsed())/1e6);
        }

        // ★ 必须复刻 CPU 的 buildTaps：fscale = max(1, scale)，radius = 3*fscale。
        //   放大时（scale<1）不夹就会把核拉宽 → 权重与 CPU 不同（缩小时间好 scale>1 看不出来）
        const float fscaleX = std::max(1.0f, float(sw) / float(tw));
        const float fscaleY = std::max(1.0f, float(sh) / float(th));
        // 支撑：Box 0.5 / Mitchell 2.0 / Lanczos = lobes（与 buildTaps 同一规则）
        scaler::Filter effF = filter;
        if (effF == scaler::Filter::Auto)
            effF = (sq == scaler::Quality::Fast) ? scaler::Filter::Box : scaler::Filter::Lanczos;
        const float support = (effF == scaler::Filter::Box) ? 0.5f
                            : (effF == scaler::Filter::Mitchell) ? 2.0f
                                                                 : float(lobes);
        const float radiusX = support * fscaleX;
        const float radiusY = support * fscaleY;

        auto runPass = [&](GLuint prog, GLuint fbo, GLuint inTex, int outW, int outH,
                           int inN, int axis, float fscale, float radius, int encode,
                           GLuint minTex = 0, GLuint maxTex = 0, int nDrawBufs = 1) {
            const int filterIdx = (effF == scaler::Filter::Mitchell) ? 2
                                 : (effF == scaler::Filter::Box) ? 1 : 0;
            f->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            const GLenum bufs[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
            f->glDrawBuffers(nDrawBufs, bufs);
            f->glViewport(0, 0, outW, outH);
            f->glUseProgram(prog);
            // ★ 必须在 glUseProgram **之后**：glUniform* 只作用于当前绑定的程序
            f->glUniform1i(f->glGetUniformLocation(prog, "uLobes"), lobes);
            f->glUniform1f(f->glGetUniformLocation(prog, "uMB"), float(params.mitchellB));
            f->glUniform1f(f->glGetUniformLocation(prog, "uMC"), float(params.mitchellC));
            if (minTex) {
                f->glActiveTexture(GL_TEXTURE1); f->glBindTexture(GL_TEXTURE_2D, minTex);
                f->glActiveTexture(GL_TEXTURE2); f->glBindTexture(GL_TEXTURE_2D, maxTex);
                f->glUniform1i(f->glGetUniformLocation(prog, "uMin"), 1);
                f->glUniform1i(f->glGetUniformLocation(prog, "uMax"), 2);
                f->glActiveTexture(GL_TEXTURE0);
            }
            f->glActiveTexture(GL_TEXTURE0);
            f->glBindTexture(GL_TEXTURE_2D, inTex);
            f->glUniform1i(f->glGetUniformLocation(prog, "uIn"), 0);
            // ★ 沿轴的那个维度：水平趟用 outW，垂直趟用 outH（写错会让缩放比全错）
            f->glUniform1i(f->glGetUniformLocation(prog, "uOutN"), axis == 0 ? outW : outH);
            f->glUniform1i(f->glGetUniformLocation(prog, "uInN"), inN);
            f->glUniform1i(f->glGetUniformLocation(prog, "uAxis"), axis);
            f->glUniform1f(f->glGetUniformLocation(prog, "uFscale"), fscale);
            f->glUniform1f(f->glGetUniformLocation(prog, "uRadius"), radius);
            f->glUniform1i(f->glGetUniformLocation(prog, "uEncode"), encode);
            f->glUniform1i(f->glGetUniformLocation(prog, "uFilter"), filterIdx);
            f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            if (const GLenum e = f->glGetError(); e != GL_NO_ERROR)
                qWarning().noquote() << QStringLiteral("[gpu] pass(axis=%1) 后 GL 错误 0x%2")
                                            .arg(axis).arg(e, 0, 16);
        };

        // ── 阶段自检：线性化 → 编码 的"直通"应当≈源图（只差 LUT/舍入）──
        //   用 lanczos 程序跑 uOutN=uInN / uRadius=0.5 → 恰好单抽头权重 1 = 直通 + 编码
        {
            runPass(progLin, fboLin, texIn, sw, sh, sw, 0, 1.0f, 0.5f, 0);
            f->glFinish();
            const GLuint texPt = makeTexture(f, sw, sh, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            const GLuint fboPt = makeFbo(f, texPt);
            runPass(progLcz, fboPt, texLin, sw, sh, sh, 1, 1.0f, 0.5f, 1);
            f->glFinish();
            QImage pt(sw, sh, QImage::Format_RGBA8888);
            f->glBindFramebuffer(GL_FRAMEBUFFER, fboPt);
            f->glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, pt.bits());
            f->glFinish();
            const QImage a = src.convertToFormat(QImage::Format_RGB32);
            const QImage b = pt.convertToFormat(QImage::Format_RGB32);
            qint64 sum = 0, n = 0, big = 0;
            for (int y = 0; y < a.height(); ++y) {
                const QRgb *ra = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                const QRgb *rb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
                for (int x = 0; x < a.width(); ++x) {
                    const int d = qAbs(qRed(ra[x])-qRed(rb[x])) + qAbs(qGreen(ra[x])-qGreen(rb[x])) + qAbs(qBlue(ra[x])-qBlue(rb[x]));
                    sum += d; n += 3; if (d > 6) ++big;
                }
            }
            qInfo().noquote() << QStringLiteral("[gpu]   阶段自检(线性化→编码 直通 vs 源图): 平均色差 %1  明显不同 %2%%")
                                     .arg(n ? double(sum)/n : -1, 0, 'f', 2)
                                     .arg(n ? 100.0*double(big)/double(n/3) : -1, 0, 'f', 2);
            f->glDeleteFramebuffers(1, &fboPt);
            f->glDeleteTextures(1, &texPt);
        }

        std::vector<double> linT, hT, vT, rdT, allT;
        QImage gpuOut;
        for (int r = 0; r < repeats; ++r) {
            QElapsedTimer t;
            t.start();
            runPass(progLin, fboLin, texIn, sw, sh, sw, 0, 1.0f, 0.5f, 0);
            f->glFinish();
            linT.push_back(double(t.nsecsElapsed())/1e6);

            t.restart();
            if (useAr) runPass(progArH, fboH, texLin, tw, sh, sw, 0, fscaleX, radiusX, 0, 0, 0, 3);
            else       runPass(progLcz, fboH, texLin, tw, sh, sw, 0, fscaleX, radiusX, 0);
            f->glFinish();
            hT.push_back(double(t.nsecsElapsed())/1e6);

            t.restart();
            if (useAr) runPass(progArV, fboOut, texH, tw, th, sh, 1, fscaleY, radiusY, 1, texHMin, texHMax, 1);
            else       runPass(progLcz, fboOut, texH, tw, th, sh, 1, fscaleY, radiusY, 1);
            f->glFinish();
            vT.push_back(double(t.nsecsElapsed())/1e6);

            t.restart();
            QImage out(tw, th, QImage::Format_RGBA8888);
            f->glBindFramebuffer(GL_FRAMEBUFFER, fboOut);
            f->glReadPixels(0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, out.bits());
            f->glFinish();
            rdT.push_back(double(t.nsecsElapsed())/1e6);

            allT.push_back(upT.back() + linT.back() + hT.back() + vT.back() + rdT.back());
            gpuOut = out.convertToFormat(QImage::Format_RGB32);
        }

        // 画质对比（与 CPU 结果逐像素）。先诊断朝向：四种翻转各算一次，取最小者
        double meanAbs = -1, pctGt2 = -1;
        QString bestOrient = QStringLiteral("?");
        QString grid;
        if (!cpuOut.isNull() && !gpuOut.isNull() && cpuOut.size() == gpuOut.size()) {
            const QImage a = cpuOut.convertToFormat(QImage::Format_RGB32);
            struct Cand { const char *name; QImage img; };
            const QVector<Cand> cands = {
                { "as-is",   gpuOut },
                { "flipV",   gpuOut.mirrored(false, true) },
                { "flipH",   gpuOut.mirrored(true, false) },
                { "flipHV",  gpuOut.mirrored(true, true) },
            };
            double best = 1e30;
            for (const Cand &c : cands) {
                qint64 sum = 0, n = 0, big = 0;
                for (int y = 0; y < a.height(); ++y) {
                    const QRgb *ra = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                    const QRgb *rb = reinterpret_cast<const QRgb *>(c.img.constScanLine(y));
                    for (int x = 0; x < a.width(); ++x) {
                        const int d = qAbs(qRed(ra[x]) - qRed(rb[x]))
                                    + qAbs(qGreen(ra[x]) - qGreen(rb[x]))
                                    + qAbs(qBlue(ra[x]) - qBlue(rb[x]));
                        sum += d; n += 3;
                        if (d > 6) ++big;
                    }
                }
                const double m = n ? double(sum) / double(n) : -1;
                if (m < best) {
                    // 4x4 块均值，看差异是"边缘"还是"全图"
                    const int bw = a.width()/4, bh = a.height()/4;
                    QStringList cells;
                    for (int by = 0; by < 4; ++by) for (int bx = 0; bx < 4; ++bx) {
                        qint64 s2 = 0, n2 = 0;
                        for (int y = by*bh; y < (by+1)*bh; ++y) {
                            const QRgb *ra = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                            const QRgb *rb = reinterpret_cast<const QRgb *>(c.img.constScanLine(y));
                            for (int x = bx*bw; x < (bx+1)*bw; ++x) {
                                s2 += qAbs(qRed(ra[x])-qRed(rb[x])) + qAbs(qGreen(ra[x])-qGreen(rb[x])) + qAbs(qBlue(ra[x])-qBlue(rb[x]));
                                n2 += 3;
                            }
                        }
                        cells << QString::number(n2 ? double(s2)/n2 : -1, 'f', 1);
                    }
                    grid = cells.join(QLatin1Char(' '));
                    // 采样像素（CPU vs GPU）
                    const int pts[5][2] = {{100,100},{1425,950},{2800,1850},{700,300},{2000,1200}};
                    QStringList samples;
                    for (auto &pt : pts) {
                        const QRgb pa = reinterpret_cast<const QRgb *>(a.constScanLine(pt[1]))[pt[0]];
                        const QRgb pb = reinterpret_cast<const QRgb *>(c.img.constScanLine(pt[1]))[pt[0]];
                        samples << QStringLiteral("(%1,%2) cpu=%3,%4,%5 gpu=%6,%7,%8")
                            .arg(pt[0]).arg(pt[1])
                            .arg(qRed(pa)).arg(qGreen(pa)).arg(qBlue(pa))
                            .arg(qRed(pb)).arg(qGreen(pb)).arg(qBlue(pb));
                    }
                    grid += QLatin1String(" || ") + samples.join(QLatin1String(" | "));
                    best = m;
                    meanAbs = m;
                    pctGt2 = n ? 100.0 * double(big) / double(n / 3) : -1;
                    bestOrient = QLatin1String(c.name);
                }
            }
        }

        Row row;
        row.name = QStringLiteral("%1[fmt%2]").arg(QFileInfo(path).fileName()).arg(int(srcFmt));
        row.src = src.size();
        row.dst = target;
        row.upload = median(upT);
        row.lin = median(linT);
        row.h = median(hT);
        row.v = median(vT);
        row.read = median(rdT);
        row.total = median(allT);
        row.cpu = median(cpuT);
        // 写入磁盘（供人工对比）：PHOTOSET_GPU_DUMP_DIR 给目录，文件名前缀用序号
        if (const QByteArray dd = qgetenv("PHOTOSET_GPU_DUMP_DIR"); !dd.isEmpty()) {
            QDir d(QString::fromLocal8Bit(dd));
            d.mkpath(QStringLiteral("."));
            const QString base = QStringLiteral("%1/%2").arg(d.absolutePath(), QString::number(rows.size()));
            if (!cpuOut.isNull()) cpuOut.save(base + QStringLiteral("-cpu.png"));
            if (!gpuOut.isNull()) gpuOut.save(base + QStringLiteral("-gpu.png"));
            if (!cpuOut.isNull() && !gpuOut.isNull() && cpuOut.size() == gpuOut.size()) {
                const QImage a = cpuOut.convertToFormat(QImage::Format_RGB32);
                const QImage b = gpuOut;
                QImage diff(a.size(), QImage::Format_RGB32);
                for (int y = 0; y < a.height(); ++y) {
                    const QRgb *ra = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                    const QRgb *rb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
                    QRgb *rd = reinterpret_cast<QRgb *>(diff.scanLine(y));
                    for (int x = 0; x < a.width(); ++x) {
                        // ×20 放大，方便肉眼确认"差异在哪"
                        const int dr = qMin(255, qAbs(qRed(ra[x])  - qRed(rb[x]))  * 20);
                        const int dg = qMin(255, qAbs(qGreen(ra[x])- qGreen(rb[x]))* 20);
                        const int db = qMin(255, qAbs(qBlue(ra[x]) - qBlue(rb[x])) * 20);
                        rd[x] = qRgb(dr, dg, db);
                    }
                }
                diff.save(base + QStringLiteral("-diff20.png"));
            }
        }

        row.meanAbs = meanAbs;
        row.pctGt2 = pctGt2;
        row.orient = bestOrient;
        row.grid = grid;
        rows.push_back(row);

        f->glDeleteFramebuffers(1, &fboLin);
        f->glDeleteFramebuffers(1, &fboH);
        f->glDeleteFramebuffers(1, &fboOut);
        GLuint del[6] = { texIn, texLin, texH, texOut, texHMin, texHMax };
        f->glDeleteTextures(useAr ? 6 : 4, del);
    }

    qInfo().noquote() << QStringLiteral("[gpu] 盒 %1x%2  档位 %3 / 滤波 %4  重复 %5 次取中位数")
                             .arg(box.width()).arg(box.height())
                             .arg(QLatin1String(scaler::qualityName(sq)),
                                  QLatin1String(scaler::filterName(filter)))
                             .arg(repeats);
    qInfo().noquote() << QStringLiteral("[gpu] 素材\t输入\t输出\t上传\t线性化\tH\tV\t回读\tGPU合计\tCPU合计\t平均色差\t明显不同%\t朝向");
    for (const Row &r : rows) {
        qInfo().noquote() << QStringLiteral("[gpu] %1\t%2x%3\t%4x%5\t%6\t%7\t%8\t%9\t%10\t%11\t%12\t%13\t%14\t%15")
                .arg(r.name)
                .arg(r.src.width()).arg(r.src.height())
                .arg(r.dst.width()).arg(r.dst.height())
                .arg(r.upload, 0, 'f', 1).arg(r.lin, 0, 'f', 1)
                .arg(r.h, 0, 'f', 1).arg(r.v, 0, 'f', 1).arg(r.read, 0, 'f', 1)
                .arg(r.total, 0, 'f', 1).arg(r.cpu, 0, 'f', 1)
                .arg(r.meanAbs, 0, 'f', 2).arg(r.pctGt2, 0, 'f', 2)
                .arg(r.orient);
        if (!r.grid.isEmpty())
            qInfo().noquote() << QStringLiteral("[gpu]   └ 4x4 块均值色差: %1").arg(r.grid);
    }
    return 0;
}

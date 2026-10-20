#include "gpuscale.h"

#include <QDebug>
#include <QFile>
#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qshader.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gpuscale {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// 每个算法展开成管线要的核参数（**唯一映射**：这里错了就是画面错了）
struct KernelSpec {
    int   filterIdx;     // 与 shader 的 flt 一致：0=lanczos 1=box 2=mitchell 3=nearest
    float support;       // 核支撑（缩小按 scale 加宽）
    int   lobes;         // 只对 lanczos 有意义
    float mB, mC;        // 只对 mitchell 有意义
    bool  useAR;         // 是否做 anti-ringing 钳位
    bool  useLinearize;  // 是否过线性光（Pixel/Nearest 直通）
};

KernelSpec specFor(Algorithm a)
{
    switch (a) {
    case Algorithm::Nearest:  return { 3, 0.5f, 3, 1.0f / 3.0f, 1.0f / 3.0f, false, false };
    case Algorithm::Bicubic:  return { 2, 2.0f, 3, 0.0f, 0.5f, false, true };   // a=-0.5
    case Algorithm::Lanczos2: return { 0, 2.0f, 2, 1.0f / 3.0f, 1.0f / 3.0f, false, true };
    case Algorithm::Lanczos3: return { 0, 3.0f, 3, 1.0f / 3.0f, 1.0f / 3.0f, true, true };
    case Algorithm::Smooth:   break;   // 不由本模块实现（见 CpuScalerItem）；落到下面的默认
    }
    // Smooth（以及任何未来值）：几何仍要算（CpuScalerItem 用同一份 makePlan），核参数不用
    return { 0, 3.0f, 3, 1.0f / 3.0f, 1.0f / 3.0f, false, true };
}

QSize clampPositive(const QSize &s, int minW = 1, int minH = 1)
{
    return QSize(std::max(minW, s.width()), std::max(minH, s.height()));
}

QRhiTextureRenderTargetDescription rtDesc1(QRhiTexture *t)
{
    return QRhiTextureRenderTargetDescription(QRhiColorAttachment(t));
}

QRhiTextureRenderTargetDescription rtDesc3(QRhiTexture *a, QRhiTexture *b, QRhiTexture *c)
{
    QRhiTextureRenderTargetDescription d;
    d.setColorAttachments({ QRhiColorAttachment(a), QRhiColorAttachment(b), QRhiColorAttachment(c) });
    return d;
}

QShader loadShader(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << QStringLiteral("[gpuscale] 打不开 shader %1（qsb 烘焙产物应在资源里）").arg(path);
        return QShader();
    }
    const QShader s = QShader::fromSerialized(f.readAll());
    if (!s.isValid())
        qWarning().noquote() << QStringLiteral("[gpuscale] shader %1 反序列化失败").arg(path);
    return s;
}

} // namespace

// ── Fit / Plan ───────────────────────────────────────────────────────

QSize fitSize(const QSize &src, const QSize &viewport, Fit fit, qreal zoom)
{
    if (src.isEmpty())
        return QSize();
    if (zoom <= 0)
        zoom = 1.0;
    const qreal w = src.width();
    const qreal h = src.height();
    qreal outW = w, outH = h;
    const qreal vw = std::max(1, viewport.width());
    const qreal vh = std::max(1, viewport.height());
    switch (fit) {
    case Fit::Original:
        break;
    case Fit::Screen: {
        // ★ 视口适配口径（原与 CPU 参照实现同式）：fitInside 就是
        //   `src.scaled(box, Qt::KeepAspectRatio)`，这里直接委托 Qt，避免自己重写取整规则而分叉
        const QSize base = src.scaled(QSize(int(vw), int(vh)), Qt::KeepAspectRatio);
        outW = base.width();
        outH = base.height();
        break;
    }
    case Fit::FitWidth:
        outW = vw;
        outH = h * (vw / w);
        break;
    case Fit::FitHeight:
        outH = vh;
        outW = w * (vh / h);
        break;
    }
    return clampPositive(QSize(qRound(outW * zoom), qRound(outH * zoom)));
}

Plan makePlan(const QSize &src, const QSize &viewport, Fit fit, qreal zoom,
              qreal panX, qreal panY, const Params &p)
{
    Plan pl;
    pl.params = p;
    pl.src = src;
    if (src.isEmpty())
        return pl;

    const QSize vs = QSize(std::max(1, viewport.width()), std::max(1, viewport.height()));
    pl.full = clampPositive(fitSize(src, vs, fit, zoom));

    // 可见窗口：图像比视口小 → 全幅；否则 = 视口大小，起点由 pan 决定（夹在合法范围内）
    const int dw = std::min(pl.full.width(), vs.width());
    const int dh = std::min(pl.full.height(), vs.height());
    pl.dst = QSize(dw, dh);
    pl.offX = (pl.full.width()  <= vs.width())  ? 0
              : qBound(0, qRound(panX), pl.full.width() - dw);
    pl.offY = (pl.full.height() <= vs.height()) ? 0
              : qBound(0, qRound(panY), pl.full.height() - dh);

    // 核宽：fscale = max(1, scale)（放大时不夹会把核拉宽 —— 12 号调研的坑）
    //   缩放比一律由**完整输出尺寸** full 决定，pan 不许改变采样
    pl.fscaleX = std::max(1.0f, float(src.width()) / float(pl.full.width()));
    pl.fscaleY = std::max(1.0f, float(src.height()) / float(pl.full.height()));

    // 算法 → 核参数（唯一映射）。**用户只有这一个旋钮**，没有第二级参数。
    const KernelSpec ks = specFor(p.algorithm);
    pl.filterIdx = ks.filterIdx;
    pl.lobes = ks.lobes;
    pl.mB = ks.mB;
    pl.mC = ks.mC;
    pl.useAR = ks.useAR;
    pl.antiring = ks.useAR ? 1.0f : 0.0f;      // 只有开 AR 的算法才有意义；强度固定
    pl.useLinearize = ks.useLinearize;

    if (p.algorithm == Algorithm::Nearest) {
        // 像素完美通路：不做线性化、不来回转色彩空间，单抽头取最近像素
        pl.fscaleX = pl.fscaleY = 1.0f;
        pl.radiusX = pl.radiusY = 0.5f;
    } else {
        pl.radiusX = ks.support * pl.fscaleX;
        pl.radiusY = ks.support * pl.fscaleY;
    }
    return pl;
}

bool Plan::operator==(const Plan &o) const
{
    return src == o.src && full == o.full && dst == o.dst && offX == o.offX && offY == o.offY
           && filterIdx == o.filterIdx && lobes == o.lobes && mB == o.mB && mC == o.mC
           && antiring == o.antiring && fscaleX == o.fscaleX && fscaleY == o.fscaleY
           && radiusX == o.radiusX && radiusY == o.radiusY && useAR == o.useAR
           && useLinearize == o.useLinearize;
}

QByteArray tapBoundsBytes(int srcN, int fullN, int startOut, int count,
                          const Params &p, bool nearest)
{
    QByteArray out;
    if (srcN <= 0 || fullN <= 0 || count <= 0)
        return out;
    out.resize(count * 4 * int(sizeof(float)));
    float *f = reinterpret_cast<float *>(out.data());

    // ★ 重采样边界规则（**double** 精度）
    const double scale = double(srcN) / double(fullN);
    const double fscale = std::max(1.0, scale);
    // 支撑与 makePlan 同源（specFor）——这里只要"能不能到边界"，不需要别的参数
    const double support = double(specFor(p.algorithm).support);
    const double radius = support * fscale;

    for (int j = 0; j < count; ++j) {
        const int i = startOut + j;                 // 完整输出里的绝对索引
        const double center = (i + 0.5) * scale - 0.5;
        int lo = 0, hi = 0;
        if (nearest) {
            lo = hi = int(std::floor(center + 0.5));
        } else {
            lo = int(std::floor(center - radius + 0.5));
            hi = int(std::ceil(center + radius - 0.5));
        }
        lo = std::max(lo, 0);
        hi = std::min(hi, srcN - 1);
        if (hi < lo)
            hi = lo;
        f[j * 4 + 0] = float(lo);
        f[j * 4 + 1] = float(hi);
        f[j * 4 + 2] = 0.0f;
        f[j * 4 + 3] = 0.0f;
    }
    return out;
}

// ── Pipeline ─────────────────────────────────────────────────────────

void PassUniforms::pack(float out[16]) const
{
    // 与 shader 的 std140 块一致：4 × vec4（p0..p2 有用，p3 保留）
    const float v[16] = { filter, axis, outN, inN,
                          fscale, radius, encode, lobes,
                          mB, mC, antiring, outOffset,
                          flipY, axisSize, 0, 0 };
    std::memcpy(out, v, sizeof(v));
}

PassUniforms passUniformsFor(const Plan &plan, int pass, bool flipY)
{
    PassUniforms u;
    u.filter = float(plan.filterIdx);
    u.lobes = float(plan.lobes);
    u.mB = plan.mB;
    u.mC = plan.mC;
    u.flipY = flipY ? 1.0f : 0.0f;
    if (pass == 1) {              // 水平趟：沿轴 = 宽
        u.axis = 0.0f;
        u.outN = float(plan.full.width());
        u.inN = float(plan.src.width());
        u.fscale = plan.fscaleX;
        u.radius = plan.radiusX;
        u.axisSize = float(plan.dst.width());
        u.encode = 0.0f;
        u.antiring = plan.antiring;
        u.outOffset = float(plan.offX);
    } else if (pass == 2) {       // 垂直趟：沿轴 = 高，回编码 + anti-ringing 钳位
        u.axis = 1.0f;
        u.axisSize = float(plan.dst.height());
        u.outN = float(plan.full.height());
        u.inN = float(plan.src.height());
        u.fscale = plan.fscaleY;
        u.radius = plan.radiusY;
        // ★ 像素完美通路（nearest）**不做线性化**：那就不许在这里回编码，
        //   否则等于对已经 sRGB 的值再做一次 linear→sRGB（实测整幅偏亮，色差 ~131）
        u.encode = plan.useLinearize ? 1.0f : 0.0f;
        u.antiring = plan.useAR ? plan.antiring : 0.0f;
        u.outOffset = float(plan.offY);
    }
    return u;
}

Pipeline::Pipeline(QRhi *rhi)
    : m_rhi(rhi)
{
    if (!m_rhi) {
        m_err = QStringLiteral("QRhi 为空");
        return;
    }

    const QShader vs = loadShader(QStringLiteral(":/gpuscale.vert.qsb"));
    const QShader fsLin = loadShader(QStringLiteral(":/gpuscale_linearize.frag.qsb"));
    const QShader fsRes = loadShader(QStringLiteral(":/gpuscale_resample.frag.qsb"));
    const QShader fsArH = loadShader(QStringLiteral(":/gpuscale_resample_ar_h.frag.qsb"));
    const QShader fsArV = loadShader(QStringLiteral(":/gpuscale_resample_ar_v.frag.qsb"));
    if (!vs.isValid() || !fsLin.isValid() || !fsRes.isValid() || !fsArH.isValid() || !fsArV.isValid()) {
        m_err = QStringLiteral("shader 资源缺失（qsb 未烘焙进资源？）");
        return;
    }
    m_vs = vs;
    m_fsLin = fsLin;
    m_fsRes = fsRes;
    m_fsArH = fsArH;
    m_fsArV = fsArV;

    m_sampler = m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_sampler->setName(QByteArrayLiteral("gpuscale.sampler"));
    if (!m_sampler->create()) {
        m_err = QStringLiteral("采样器创建失败");
        return;
    }

    // 全屏四边形（triangle strip 4 顶点）——（与早期实验工具同序）
    static const float kQuad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    m_vbuf = m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad));
    m_vbuf->setName(QByteArrayLiteral("gpuscale.quad"));
    if (!m_vbuf->create()) {
        m_err = QStringLiteral("顶点缓冲创建失败");
        return;
    }
    m_vbufPending = true;

    auto makeUbo = [this](const char *name) {
        QRhiBuffer *b = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64);
        b->setName(QByteArray(name));
        if (!b->create()) {
            delete b;
            return static_cast<QRhiBuffer *>(nullptr);
        }
        return b;
    };
    m_uboLin = makeUbo("gpuscale.ubo.lin");
    m_uboResH = makeUbo("gpuscale.ubo.resH");
    m_uboResV = makeUbo("gpuscale.ubo.resV");
    m_uboArH = makeUbo("gpuscale.ubo.arH");
    m_uboArV = makeUbo("gpuscale.ubo.arV");
    if (!m_uboLin || !m_uboResH || !m_uboResV || !m_uboArH || !m_uboArV) {
        m_err = QStringLiteral("uniform 缓冲创建失败");
        return;
    }

    // 管线模板：1×1 的三种附件组合（管线与尺寸无关，只与附件格式/数量有关）
    m_tpl16 = m_rhi->newTexture(QRhiTexture::RGBA16F, QSize(1, 1), 1, QRhiTexture::RenderTarget);
    m_tpl16b = m_rhi->newTexture(QRhiTexture::RGBA16F, QSize(1, 1), 1, QRhiTexture::RenderTarget);
    m_tpl16c = m_rhi->newTexture(QRhiTexture::RGBA16F, QSize(1, 1), 1, QRhiTexture::RenderTarget);
    m_tpl8 = m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1), 1, QRhiTexture::RenderTarget);
    if (!m_tpl16->create() || !m_tpl16b->create() || !m_tpl16c->create() || !m_tpl8->create()) {
        m_err = QStringLiteral("管线模板纹理创建失败");
        return;
    }
    m_tplRt16 = m_rhi->newTextureRenderTarget(rtDesc1(m_tpl16));
    m_tplRt3 = m_rhi->newTextureRenderTarget(rtDesc3(m_tpl16, m_tpl16b, m_tpl16c));
    m_tplRt8 = m_rhi->newTextureRenderTarget(rtDesc1(m_tpl8));
    m_tplRp16 = m_tplRt16->newCompatibleRenderPassDescriptor();
    m_tplRp3 = m_tplRt3->newCompatibleRenderPassDescriptor();
    m_tplRp8 = m_tplRt8->newCompatibleRenderPassDescriptor();
    m_tplRt16->setRenderPassDescriptor(m_tplRp16);
    m_tplRt3->setRenderPassDescriptor(m_tplRp3);
    m_tplRt8->setRenderPassDescriptor(m_tplRp8);
    if (!m_tplRt16->create() || !m_tplRt3->create() || !m_tplRt8->create()) {
        m_err = QStringLiteral("管线模板 FBO 创建失败");
        return;
    }

    // 管线在第一次 render 时（SRB 有内容之后）创建，见 ensureBindings
}

Pipeline::~Pipeline()
{
    releaseAll();
}

void Pipeline::invalidateSrbs()
{
    // ★ 只标脏，**不销毁也不换对象**：QRhiGraphicsPipeline 在 create() 时持有
    //   QRhiShaderResourceBindings 的指针，换对象会让管线指向悬垂指针。
    //   内容重建走 QRhi 的资源复用约定（destroy + setBindings + create 同一对象）。
    m_srbDirty = true;
}

void Pipeline::destroyLin()
{
    if (m_rtLin) { m_rtLin->destroy(); delete m_rtLin; m_rtLin = nullptr; }
    if (m_linTex) { m_linTex->destroy(); delete m_linTex; m_linTex = nullptr; }
    m_linSize = QSize();
}

void Pipeline::destroyH()
{
    if (m_rtH) { m_rtH->destroy(); delete m_rtH; m_rtH = nullptr; }
    if (m_rtHAr) { m_rtHAr->destroy(); delete m_rtHAr; m_rtHAr = nullptr; }
    if (m_hTex) { m_hTex->destroy(); delete m_hTex; m_hTex = nullptr; }
    if (m_hMinTex) { m_hMinTex->destroy(); delete m_hMinTex; m_hMinTex = nullptr; }
    if (m_hMaxTex) { m_hMaxTex->destroy(); delete m_hMaxTex; m_hMaxTex = nullptr; }
    m_hSize = QSize();
    m_hAr = false;
}

void Pipeline::destroyBounds()
{
    if (m_boundsH) { m_boundsH->destroy(); delete m_boundsH; m_boundsH = nullptr; }
    if (m_boundsV) { m_boundsV->destroy(); delete m_boundsV; m_boundsV = nullptr; }
    m_boundsValid = false;
    m_boundsUploaded = false;
}

void Pipeline::destroyOut()
{
    if (m_rtOut) { m_rtOut->destroy(); delete m_rtOut; m_rtOut = nullptr; }
    if (m_outTex) { m_outTex->destroy(); delete m_outTex; m_outTex = nullptr; }
    m_outSize = QSize();
}

void Pipeline::releaseAll()
{
    m_srbDirty = true;
    destroyLin();
    destroyH();
    destroyOut();
    if (m_srcTex) { m_srcTex->destroy(); delete m_srcTex; m_srcTex = nullptr; }
    m_srcTexSize = QSize();
    auto killPipe = [](QRhiGraphicsPipeline *&p) {
        if (p) { p->destroy(); delete p; p = nullptr; }
    };
    killPipe(m_pLin);
    killPipe(m_pResH);
    killPipe(m_pResV);
    killPipe(m_pArH);
    killPipe(m_pArV);
    auto killSrb = [](QRhiShaderResourceBindings *&s) {
        if (s) { s->destroy(); delete s; s = nullptr; }
    };
    killSrb(m_srbLin);
    killSrb(m_srbResH);
    killSrb(m_srbResV);
    killSrb(m_srbArH);
    killSrb(m_srbArV);
    destroyBounds();
    auto killRt = [](QRhiTextureRenderTarget *&rt) {
        if (rt) { rt->destroy(); delete rt; rt = nullptr; }
    };
    killRt(m_tplRt16);
    killRt(m_tplRt3);
    killRt(m_tplRt8);
    auto killRp = [](QRhiRenderPassDescriptor *&rp) {
        if (rp) { rp->destroy(); delete rp; rp = nullptr; }
    };
    killRp(m_tplRp16);
    killRp(m_tplRp3);
    killRp(m_tplRp8);
    auto killTex = [](QRhiTexture *&t) {
        if (t) { t->destroy(); delete t; t = nullptr; }
    };
    killTex(m_tpl16);
    killTex(m_tpl16b);
    killTex(m_tpl16c);
    killTex(m_tpl8);
    auto killBuf = [](QRhiBuffer *&b) {
        if (b) { b->destroy(); delete b; b = nullptr; }
    };
    killBuf(m_vbuf);
    killBuf(m_uboLin);
    killBuf(m_uboResH);
    killBuf(m_uboResV);
    killBuf(m_uboArH);
    killBuf(m_uboArV);
    if (m_sampler) { m_sampler->destroy(); delete m_sampler; m_sampler = nullptr; }
}

void Pipeline::setSourceImage(const QImage &img)
{
    if (img.isNull()) {
        m_src = QImage();
        m_srcSize = QSize();
        ++m_srcGen;
        return;
    }
    // 统一成 RGBA8888：① 与 CPU 侧同口径（CPU 也先 convertToFormat）
    //                ② QRhiTexture 的 RGBA8 上传要求格式匹配
    const QImage conv = (img.format() == QImage::Format_RGBA8888)
                            ? img
                            : img.convertToFormat(QImage::Format_RGBA8888);
    if (conv.size() == m_srcSize && conv.constBits() == m_src.constBits())
        return;   // 同一个缓冲区（隐式共享）→ 不必重传
    m_src = conv;
    m_srcSize = conv.size();
    ++m_srcGen;
}

bool Pipeline::ensureSource(QRhiResourceUpdateBatch *rub)
{
    if (m_src.isNull() || m_srcSize.isEmpty())
        return false;
    if (!m_srcTex || m_srcTexSize != m_srcSize) {
        if (m_srcTex) { m_srcTex->destroy(); delete m_srcTex; m_srcTex = nullptr; }
        m_srcTex = m_rhi->newTexture(QRhiTexture::RGBA8, m_srcSize, 1, QRhiTexture::UsedAsTransferSource);
        m_srcTex->setName(QByteArrayLiteral("gpuscale.src"));
        if (!m_srcTex->create()) {
            m_lastErr = QStringLiteral("源纹理创建失败 %1x%2").arg(m_srcSize.width()).arg(m_srcSize.height());
            return false;
        }
        m_srcTexSize = m_srcSize;
        m_uploadedGen = ~0ull;
    }
    if (m_uploadedGen != m_srcGen) {
        rub->uploadTexture(m_srcTex, m_src);
        m_uploadedGen = m_srcGen;
    }
    return true;
}

bool Pipeline::ensurePipelines(QRhiRenderPassDescriptor *finalRp)
{
    if (m_pLin)
        return true;
    if (!m_tplRt16 || !m_tplRt3) {
        m_lastErr = QStringLiteral("管线模板缺失，无法建管线");
        return false;
    }
    if (!finalRp)
        finalRp = m_tplRp8;
    // ★ QRhi 的硬性要求：graphics pipeline 必须有 QRhiShaderResourceBindings
    //   （qrhi.cpp: "Cannot build a graphics pipeline without QRhiShaderResourceBindings"），
    //   而且 GL 后端在 create() 时会按 shader 声明的资源去 SRB 里找绑定 ⇒ 必须**先有内容**。
    //   所以管线一律在 ensureBindings() 把 SRB 填好之后才建。
    if (!m_srbLin || !m_srbResH || !m_srbResV || !m_srbArH || !m_srbArV) {
        m_lastErr = QStringLiteral("SRB 尚未创建，不能建管线");
        return false;
    }

    QRhiVertexInputLayout layout;
    layout.setBindings({ QRhiVertexInputBinding(2 * sizeof(float)) });
    layout.setAttributes({ QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0) });

    auto build = [&](QRhiGraphicsPipeline *p, const QShader &vs, const QShader &fs,
                     QRhiRenderPassDescriptor *rp, int attachments, const char *name,
                     QRhiShaderResourceBindings *srb) {
        p->setName(QByteArray(name));
        p->setShaderStages({ QRhiShaderStage(QRhiShaderStage::Vertex, vs),
                             QRhiShaderStage(QRhiShaderStage::Fragment, fs) });
        p->setVertexInputLayout(layout);
        p->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        p->setCullMode(QRhiGraphicsPipeline::None);
        p->setDepthTest(false);
        p->setDepthWrite(false);
        switch (attachments) {
        case 1: p->setTargetBlends({ QRhiGraphicsPipeline::TargetBlend() }); break;
        case 3: p->setTargetBlends({ QRhiGraphicsPipeline::TargetBlend(),
                                     QRhiGraphicsPipeline::TargetBlend(),
                                     QRhiGraphicsPipeline::TargetBlend() }); break;
        default: break;
        }
        p->setRenderPassDescriptor(rp);
        p->setShaderResourceBindings(srb);      // ★ 不能省（哪怕这一趟没有资源）
    };

    m_pLin = m_rhi->newGraphicsPipeline();
    build(m_pLin, m_vs, m_fsLin, m_tplRp16, 1, "gpuscale.pipe.linearize", m_srbLin);
    m_pResH = m_rhi->newGraphicsPipeline();
    build(m_pResH, m_vs, m_fsRes, m_tplRp16, 1, "gpuscale.pipe.resampleH", m_srbResH);
    m_pResV = m_rhi->newGraphicsPipeline();
    build(m_pResV, m_vs, m_fsRes, finalRp, 1, "gpuscale.pipe.resampleV", m_srbResV);
    m_pArH = m_rhi->newGraphicsPipeline();
    build(m_pArH, m_vs, m_fsArH, m_tplRp3, 3, "gpuscale.pipe.arH", m_srbArH);
    m_pArV = m_rhi->newGraphicsPipeline();
    build(m_pArV, m_vs, m_fsArV, finalRp, 1, "gpuscale.pipe.arV", m_srbArV);
    m_finalRp = finalRp;

    struct PipeRef { QRhiGraphicsPipeline **p; const char *name; };
    const PipeRef refs[5] = { { &m_pLin, "lin" }, { &m_pResH, "resH" }, { &m_pResV, "resV" },
                              { &m_pArH, "arH" }, { &m_pArV, "arV" } };
    for (const PipeRef &r : refs) {
        if (!(*r.p)->create()) {
            m_lastErr = QStringLiteral("管线创建失败：%1").arg(QLatin1String(r.name));
            return false;
        }
    }
    return true;
}

bool Pipeline::ensureFinalPipelines(QRhiRenderPassDescriptor *rp)
{
    if (!rp) {
        m_lastErr = QStringLiteral("渲染目标没有 renderPassDescriptor");
        return false;
    }
    if (m_pResV && m_finalRp == rp)
        return true;
    // QRhi 的资源复用约定：改参数 + create() = 隐式 destroy + create，引用它的对象无需改动
    //   （SRB 指针不变；这里只换渲染目标的附件结构）
    if (m_pResV) {
        m_pResV->setRenderPassDescriptor(rp);
        if (!m_pResV->create()) {
            m_lastErr = QStringLiteral("最终趟管线重建失败（resample V）");
            return false;
        }
    }
    if (m_pArV) {
        m_pArV->setRenderPassDescriptor(rp);
        if (!m_pArV->create()) {
            m_lastErr = QStringLiteral("最终趟管线重建失败（anti-ringing V）");
            return false;
        }
    }
    m_finalRp = rp;
    return true;
}

bool Pipeline::ensureTargets(const Plan &plan, bool needOwnOut)
{
    const QSize linSize(plan.src.width(), plan.src.height());
    const QSize hSize(plan.dst.width(), plan.src.height());
    const QSize outSize(plan.dst);
    const bool ar = plan.useAR;

    const bool linKeep = (linSize == m_linSize) && m_linTex && m_rtLin;
    const bool hKeep = (hSize == m_hSize) && (ar == m_hAr) && m_hTex
                       && (ar ? (m_rtHAr != nullptr) : (m_rtH != nullptr));
    const bool outKeep = !needOwnOut || ((outSize == m_outSize) && m_outTex && m_rtOut);
    // 抽头边界纹理：只在 (src, full, 核, nearest) 变化时重建 —— pan 不重建（边界按**完整输出**索引）
    BoundsKey bk;
    bk.src = plan.src;
    bk.full = plan.full;
    bk.dst = plan.dst;
    bk.offX = plan.offX;
    bk.offY = plan.offY;
    bk.filterIdx = plan.filterIdx;
    bk.lobes = plan.lobes;
    bk.nearest = !plan.useLinearize;
    const bool boundsKeep = m_boundsValid && (bk == m_boundsKey) && m_boundsH && m_boundsV;
    if (linKeep && hKeep && outKeep && boundsKeep)
        return true;

    // 纹理要换 → 引用了它们的 SRB 内容必须重建（在 ensureBindings 里）
    invalidateSrbs();
    if (!linKeep)
        destroyLin();
    if (!hKeep)
        destroyH();
    if (!outKeep)
        destroyOut();
    if (!boundsKeep) {
        destroyBounds();
        m_boundsKey = bk;
        m_boundsValid = true;
        // 纹理只按**可见窗口**大小（放大到 8× 时 full 可能几万像素，超过最大纹理尺寸）
        m_boundsH = m_rhi->newTexture(QRhiTexture::RGBA32F, QSize(qMax(1, plan.dst.width()), 1), 1);
        m_boundsV = m_rhi->newTexture(QRhiTexture::RGBA32F, QSize(qMax(1, plan.dst.height()), 1), 1);
        m_boundsH->setName(QByteArrayLiteral("gpuscale.boundsH"));
        m_boundsV->setName(QByteArrayLiteral("gpuscale.boundsV"));
        if (!m_boundsH->create() || !m_boundsV->create()) {
            m_lastErr = QStringLiteral("抽头边界纹理创建失败（RGBA32F 采样是否受支持？）");
            return false;
        }
        m_boundsUploaded = false;      // 内容在 render 里随 rub 上传
        m_srbDirty = true;
    }

    if (!m_linTex) {
        m_linSize = linSize;
        m_linTex = m_rhi->newTexture(QRhiTexture::RGBA16F, m_linSize, 1, QRhiTexture::RenderTarget);
        m_linTex->setName(QByteArrayLiteral("gpuscale.lin"));
        if (!m_linTex->create()) { m_lastErr = QStringLiteral("线性化纹理创建失败"); return false; }
        m_rtLin = m_rhi->newTextureRenderTarget(rtDesc1(m_linTex));
        m_rtLin->setName(QByteArrayLiteral("gpuscale.rt.lin"));
        if (!m_rtLin->create()) { m_lastErr = QStringLiteral("线性化 FBO 创建失败"); return false; }
    }

    if (!m_hTex) {
        m_hSize = hSize;
        m_hAr = ar;
        m_hTex = m_rhi->newTexture(QRhiTexture::RGBA16F, m_hSize, 1, QRhiTexture::RenderTarget);
        m_hTex->setName(QByteArrayLiteral("gpuscale.h"));
        if (!m_hTex->create()) { m_lastErr = QStringLiteral("水平趟纹理创建失败"); return false; }
        if (m_hAr) {
            m_hMinTex = m_rhi->newTexture(QRhiTexture::RGBA16F, m_hSize, 1, QRhiTexture::RenderTarget);
            m_hMaxTex = m_rhi->newTexture(QRhiTexture::RGBA16F, m_hSize, 1, QRhiTexture::RenderTarget);
            m_hMinTex->setName(QByteArrayLiteral("gpuscale.hmin"));
            m_hMaxTex->setName(QByteArrayLiteral("gpuscale.hmax"));
            if (!m_hMinTex->create() || !m_hMaxTex->create()) {
                m_lastErr = QStringLiteral("anti-ringing min/max 纹理创建失败");
                return false;
            }
            m_rtHAr = m_rhi->newTextureRenderTarget(rtDesc3(m_hTex, m_hMinTex, m_hMaxTex));
            m_rtHAr->setName(QByteArrayLiteral("gpuscale.rt.h.ar"));
            if (!m_rtHAr->create()) { m_lastErr = QStringLiteral("anti-ringing FBO 创建失败"); return false; }
        } else {
            m_rtH = m_rhi->newTextureRenderTarget(rtDesc1(m_hTex));
            m_rtH->setName(QByteArrayLiteral("gpuscale.rt.h"));
            if (!m_rtH->create()) { m_lastErr = QStringLiteral("水平趟 FBO 创建失败"); return false; }
        }
    }

    if (needOwnOut && !m_outTex) {
        m_outSize = outSize;
        m_outTex = m_rhi->newTexture(QRhiTexture::RGBA8, m_outSize, 1,
                                     QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
        m_outTex->setName(QByteArrayLiteral("gpuscale.out"));
        if (!m_outTex->create()) { m_lastErr = QStringLiteral("输出纹理创建失败"); return false; }
        m_rtOut = m_rhi->newTextureRenderTarget(rtDesc1(m_outTex));
        m_rtOut->setName(QByteArrayLiteral("gpuscale.rt.out"));
        if (!m_rtOut->create()) { m_lastErr = QStringLiteral("输出 FBO 创建失败"); return false; }
    }
    m_srbDirty = true;
    return true;
}

bool Pipeline::ensureBindings(bool useLin, QRhiRenderPassDescriptor *finalRp)
{
    // ① SRB 对象**只建一次**（管线 create() 时持有它们的指针，之后绝不能换对象）
    if (!m_srbLin) {
        struct SrbRef { QRhiShaderResourceBindings **srb; const char *name; };
        const SrbRef refs[5] = { { &m_srbLin, "lin" }, { &m_srbResH, "resH" }, { &m_srbResV, "resV" },
                                 { &m_srbArH, "arH" }, { &m_srbArV, "arV" } };
        for (const SrbRef &r : refs) {
            *r.srb = m_rhi->newShaderResourceBindings();
            (*r.srb)->setName(QByteArray("gpuscale.srb.") + r.name);
        }
        m_srbDirty = true;
    }

    // ② 内容变了（纹理换过 / nearest 通路换输入纹理）→ 重建到**同一批对象**上
    if (m_srbDirty || m_srbUseLin != useLin) {
        if (!m_srcTex || !m_linTex || !m_hTex)
            return false;
        const QRhiShaderResourceBinding::StageFlags fs = QRhiShaderResourceBinding::FragmentStage;
        if (!m_boundsH || !m_boundsV) {
            m_lastErr = QStringLiteral("抽头边界纹理缺失");
            return false;
        }
        // 绑定号：0 = UBO，1 = 输入纹理，2 = 抽头边界，3/4 = anti-ringing 的 min/max
        struct SrbSpec { QRhiShaderResourceBindings *srb; QRhiBuffer *ubo; QRhiTexture *a; QRhiTexture *bounds;
                         QRhiTexture *minT; QRhiTexture *maxT; };
        const SrbSpec specs[5] = {
            { m_srbLin,  m_uboLin,  m_srcTex, nullptr, nullptr, nullptr },
            { m_srbResH, m_uboResH, useLin ? m_linTex : m_srcTex, m_boundsH, nullptr, nullptr },
            { m_srbResV, m_uboResV, m_hTex,   m_boundsV, nullptr, nullptr },
            { m_srbArH,  m_uboArH,  m_linTex, m_boundsH, nullptr, nullptr },
            { m_srbArV,  m_uboArV,  m_hTex,   m_boundsV, m_hMinTex, m_hMaxTex },
        };
        for (const SrbSpec &s : specs) {
            s.srb->destroy();   // 复用同一对象（QRhi 文档的 reuse 约定）
            QList<QRhiShaderResourceBinding> binds;
            binds << QRhiShaderResourceBinding::uniformBuffer(0, fs, s.ubo, 0, 64);
            binds << QRhiShaderResourceBinding::sampledTexture(1, fs, s.a, m_sampler);
            if (s.bounds)
                binds << QRhiShaderResourceBinding::sampledTexture(2, fs, s.bounds, m_sampler);
            if (s.minT)
                binds << QRhiShaderResourceBinding::sampledTexture(3, fs, s.minT, m_sampler);
            if (s.maxT)
                binds << QRhiShaderResourceBinding::sampledTexture(4, fs, s.maxT, m_sampler);
            s.srb->setBindings(binds.cbegin(), binds.cend());
            if (!s.srb->create()) {
                m_lastErr = QStringLiteral("shader resource bindings 创建失败");
                return false;
            }
        }
        m_srbDirty = false;
        m_srbUseLin = useLin;
    }

    // ③ 管线：必须在 SRB 有内容之后建（QRhi 硬性要求）
    if (!ensurePipelines(finalRp))
        return false;
    // ④ 调用方的渲染目标换了（QQuickRhiItem 的 RT ↔ 离屏 RT）→ 只重建最终两趟
    return ensureFinalPipelines(finalRp);
}

bool Pipeline::render(QRhiCommandBuffer *cb, QRhiResourceUpdateBatch *rub, const Plan &plan,
                      QRhiRenderTarget *finalRT, QRhiReadbackResult *readback)
{
    m_passLog.clear();
    m_lastErr.clear();
    if (!m_err.isEmpty())
        return false;
    if (!plan.valid()) {
        m_lastErr = QStringLiteral("无效的 Plan（源或目标尺寸为空）");
        return false;
    }
    if (plan.src != m_srcSize) {
        m_lastErr = QStringLiteral("Plan 的源尺寸 %1x%2 与已设源图 %3x%4 不一致")
                        .arg(plan.src.width()).arg(plan.src.height())
                        .arg(m_srcSize.width()).arg(m_srcSize.height());
        return false;
    }
    if (!finalRT && plan.dst.isEmpty()) {
        m_lastErr = QStringLiteral("离屏输出尺寸为空");
        return false;
    }

    // 目标纹理（尺寸驱动；尺寸不变则复用，不跨帧保存缩放结果，只是一帧内的工作区）
    if (!ensureTargets(plan, finalRT == nullptr))
        return false;
    if (!ensureSource(rub))
        return false;
    // SRB（填好内容）→ 管线（必须带 SRB 建）；最终两趟按**实际渲染目标**的附件结构建
    if (!ensureBindings(plan.useLinearize, finalRT ? finalRT->renderPassDescriptor() : m_tplRp8))
        return false;
    if (m_vbufPending) {
        static const float kQuad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        rub->uploadStaticBuffer(m_vbuf, 0, sizeof(kQuad), kQuad);
        m_vbufPending = false;
    }
    if (!m_boundsUploaded) {
        const QByteArray hb = tapBoundsBytes(plan.src.width(), plan.full.width(), plan.offX,
                                            plan.dst.width(), plan.params, !plan.useLinearize);
        const QByteArray vb = tapBoundsBytes(plan.src.height(), plan.full.height(), plan.offY,
                                            plan.dst.height(), plan.params, !plan.useLinearize);
        if (hb.isEmpty() || vb.isEmpty()) {
            m_lastErr = QStringLiteral("抽头边界数据生成失败");
            return false;
        }
        rub->uploadTexture(m_boundsH, QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                          0, 0, QRhiTextureSubresourceUploadDescription(hb))));
        rub->uploadTexture(m_boundsV, QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                          0, 0, QRhiTextureSubresourceUploadDescription(vb))));
        m_boundsUploaded = true;
    }
    if (!m_rtLin || !m_hTex || (!m_rtH && !m_rtHAr) || (!finalRT && !m_rtOut)) {
        m_lastErr = QStringLiteral("渲染资源不完整");
        return false;
    }

    const QRhiCommandBuffer::VertexInput vi(m_vbuf, 0);
    const QColor clear(0, 0, 0, 255);
    const QRhiDepthStencilClearValue dsClear(1.0f, 0);

    auto pass = [&](QRhiRenderTarget *rt, QRhiGraphicsPipeline *pipe, QRhiShaderResourceBindings *srb,
                    QRhiBuffer *ubo, const PassUniforms &u, QRhiResourceUpdateBatch *rubForPass,
                    QRhiReadbackResult *rbAfter = nullptr) {
        u.pack(m_uboScratch);
        QRhiResourceUpdateBatch *r = rubForPass ? rubForPass : m_rhi->nextResourceUpdateBatch();
        r->updateDynamicBuffer(ubo, 0, 64, m_uboScratch);
        cb->beginPass(rt, clear, dsClear, r);
        cb->setGraphicsPipeline(pipe);
        cb->setShaderResources(srb);
        cb->setVertexInput(0, 1, &vi);
        const QSize px = rt->pixelSize();
        cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
        cb->draw(4);
        if (rbAfter && m_outTex) {
            QRhiResourceUpdateBatch *rbRub = m_rhi->nextResourceUpdateBatch();
            rbRub->readBackTexture(QRhiReadbackDescription(m_outTex), rbAfter);
            cb->endPass(rbRub);
        } else {
            cb->endPass();
        }
    };

    int nPass = 0;
    if (plan.useLinearize) {
        PassUniforms u;
        pass(m_rtLin, m_pLin, m_srbLin, m_uboLin, u, nPass == 0 ? rub : nullptr);
        ++nPass;
    }

    // ② 水平趟：outN = 完整输出宽度，inN = 源宽，offset = 可见窗口起点
    const bool flipY = m_rhi->isYUpInFramebuffer();
    const PassUniforms h = passUniformsFor(plan, 1, flipY);
    pass(plan.useAR ? m_rtHAr : m_rtH, plan.useAR ? m_pArH : m_pResH,
         plan.useAR ? m_srbArH : m_srbResH, plan.useAR ? m_uboArH : m_uboResH,
         h, nPass == 0 ? rub : nullptr);
    ++nPass;

    // ③ 垂直趟 + 回编码：outN = 完整输出高度，inN = 源高，offset = 可见窗口起点
    const PassUniforms v = passUniformsFor(plan, 2, flipY);
    QRhiRenderTarget *dst = finalRT ? finalRT : m_rtOut;
    pass(dst, plan.useAR ? m_pArV : m_pResV, plan.useAR ? m_srbArV : m_srbResV,
         plan.useAR ? m_uboArV : m_uboResV, v, nPass == 0 ? rub : nullptr, readback);
    ++nPass;

    m_passLog = QStringLiteral("%1 趟：线性化=%2 H(%3x%4) V(%5x%6) 核=%7/半径=%8")
                    .arg(nPass)
                    .arg(plan.useLinearize ? QStringLiteral("有") : QStringLiteral("跳过nearest"))
                    .arg(plan.full.width()).arg(plan.full.height())
                    .arg(plan.dst.width()).arg(plan.dst.height())
                    .arg(plan.filterIdx)
                    .arg(double(plan.radiusX), 0, 'f', 2);
    return true;
}

const char *algorithmKey(Algorithm a)
{
    switch (a) {
    case Algorithm::Nearest:  return "nearest";
    case Algorithm::Smooth:   return "smooth";
    case Algorithm::Bicubic:  return "bicubic";
    case Algorithm::Lanczos2: return "lanczos2";
    case Algorithm::Lanczos3: return "lanczos3";
    }
    return "lanczos3";
}

const char *algorithmLabel(Algorithm a)
{
    switch (a) {
    case Algorithm::Nearest:  return "Nearest";
    case Algorithm::Smooth:   return "Smooth (Qt)";   // 唯一由 Qt 实现的一项，标签要说明来源
    case Algorithm::Bicubic:  return "Bicubic";
    case Algorithm::Lanczos2: return "Lanczos 2";
    case Algorithm::Lanczos3: return "Lanczos 3";
    }
    return "Lanczos 3";
}

Algorithm algorithmFromKey(const QString &key, Algorithm fallback)
{
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("nearest"))  return Algorithm::Nearest;
    if (k == QLatin1String("smooth"))   return Algorithm::Smooth;
    if (k == QLatin1String("bicubic"))  return Algorithm::Bicubic;
    if (k == QLatin1String("lanczos2")) return Algorithm::Lanczos2;
    if (k == QLatin1String("lanczos3")) return Algorithm::Lanczos3;
    return fallback;
}

} // namespace gpuscale

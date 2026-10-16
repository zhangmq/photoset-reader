#pragma once

#include <QImage>
#include <QSize>
#include <QString>

// QRhi / QShader（Qt 的私有包含路径；QQuickRhiItem 文档同样要求 link Qt6::GuiPrivate）
#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qshader.h>

#include "scaler.h"

// ══ GPU 缩放渲染路径（产品路径的算法核心）══════════════════════════════════
//
// 与 `src/scaler.cpp`（CPU 参照实现）**逐像素等价**，与实验工具 `src/gpubench.cpp`
// 的 shader 数学**逐字一致**（见 docs/research/12）：
//
//     ① 线性化（sRGB8 → 线性 float） → ② 水平趟 → ③ 垂直趟（+ anti-ringing 钳位）→ 线性→sRGB
//
// 三段各自一个 FBO，中间纹理 RGBA16F，输出 RGBA8。
//
// ## 为什么把「管线」与「Qt Quick 项」分开
//   `Pipeline` 只依赖 QRhi（不依赖 Qt Quick）；`GpuScalerItem`（QQuickRhiItem）与
//   离屏等价性对照（`gpuscalecheck.cpp`）都调用**同一个** `Pipeline` + 同一份 `Plan`，
//   所以「验过的」就是「产品跑的」——不是两份实现（AGENTS.md「算法单一事实源」）。
//
// ## 不缓存缩放结果（设计 13 §八）
//   中间纹理只是**一帧内的**工作区，不跨帧保存「上一帧的缩放结果」，也不按尺寸/参数缓存。
//   静态时 Qt Quick 不重绘 ⇒ 没有每帧成本；交互缩放/平移时参数每帧都变 ⇒ 缓存必然失效。
//
// ## 无锁
//   本文件不出现任何互斥量：源图由 GUI 线程持有，渲染线程在 QQuickRhiItem 的
//   `synchronize()`（GUI 线程被阻塞）里取走引用（QImage 隐式共享，零拷贝）。
namespace gpuscale {

// ── 视口适配：原始 / 适应屏幕 / 适应宽度 / 适应高度（设计 13 §八）────────
enum class Fit {
    Original,    // 原始大小（100%）
    Screen,      // 适应屏幕（整幅装进视口）
    FitWidth,    // 适应宽度
    FitHeight,   // 适应高度
};

// 目标盒：把 src 按 fit + zoom 得到的**完整**输出尺寸（设备像素，未裁剪）
QSize fitSize(const QSize &src, const QSize &viewport, Fit fit, qreal zoom);

// ── 一次渲染的参数计划（**唯一事实源**）────────────────────────────────
//   `full` 决定缩放比与核宽（← pan 不许改变采样），`dst` 是本次真正渲染出来的可见区域。
struct Plan {
    scaler::Params params;
    QSize src;                       // 源图像素尺寸
    QSize full;                      // 完整缩放后尺寸（scale = src/full）
    QSize dst;                       // 本次渲染尺寸 = min(full - off, 视口)
    int   offX = 0, offY = 0;        // 可见窗口在 full 中的起点（输出像素）
    int   filterIdx = 0;             // 0=lanczos 1=box 2=mitchell 3=nearest
    int   lobes = 3;
    float mB = 1.0f / 3.0f;
    float mC = 1.0f / 3.0f;
    float antiring = 1.0f;
    float fscaleX = 1.0f, fscaleY = 1.0f;
    float radiusX = 3.0f, radiusY = 3.0f;
    bool  useAR = false;             // quality == Best（三附件 H 趟 + V 趟钳位）
    bool  useLinearize = true;       // mode == Nearest 时为 false（像素完美：不来回转换色彩空间）

    bool valid() const { return !src.isEmpty() && !dst.isEmpty() && !full.isEmpty(); }
    bool operator==(const Plan &o) const;
    bool operator!=(const Plan &o) const { return !(*this == o); }
};

// 由「源尺寸 + 视口 + fit/zoom/pan + scaler 参数」算出一次渲染的完整计划
Plan makePlan(const QSize &src, const QSize &viewport, Fit fit, qreal zoom,
              qreal panX, qreal panY, const scaler::Params &p);

// ── 每趟的 uniform（**单一事实源**：产品管线与离屏自检都用这一个）──────────
//   与 shader 的 std140 块一致：p0=(filter,axis,outN,inN) p1=(fscale,radius,encode,lobes)
//   p2=(mitchellB,mitchellC,antiring,outOffset) p3=(flipY,axisSize,0,0)
//
//   ★ flipY / axisSize 是**跨后端朝向**的关键（有证据，不是猜的）：
//     我们的管线把「图像第 0 行」写进**纹理行 0**（QImage 原样上传 + 用 gl_FragCoord 推索引），
//     而 QQuickRhiItem 合成时按后端选 UV 变换（qquickrhiitem.cpp）：
//       isYUpInFramebuffer() 为真（OpenGL）→ 默认 mirrorVertically=false 时**镜像 UV**
//                             为假（Vulkan/D3D/Metal）→ 不镜像
//     于是 GL 上「item 顶」取的是纹理**最后一行** ⇒ 垂直趟必须把索引翻过来，
//     否则 GL 上画面上下颠倒（Vulkan 等则不需要翻）。
//     ⇒ flipY = rhi->isYUpInFramebuffer()；axisSize = 本趟渲染目标沿该轴的像素数。
struct PassUniforms {
    float filter = 0, axis = 0, outN = 0, inN = 0;
    float fscale = 1, radius = 1, encode = 0, lobes = 3;
    float mB = 1.0f / 3.0f, mC = 1.0f / 3.0f, antiring = 0, outOffset = 0;
    float flipY = 0, axisSize = 0;
    void pack(float out[16]) const;
};

// pass: 0 = 线性化、1 = 水平趟、2 = 垂直趟（与 Pipeline::render 的趟序一致）
PassUniforms passUniformsFor(const Plan &plan, int pass, bool flipY);

// ── 抽头边界（lo/hi）**由 CPU 算好**，GPU 只读 ─────────────────────────────
//   ★ 为什么不能让 shader 自己算（2026-10-12 真机实测，NVIDIA 615.78.08）：
//     CPU 参照实现 `scaler::buildTaps` 用 **double**：
//       center = (i+0.5)*scale-0.5; lo = max(floor(center-radius+0.5),0);
//       hi = min(ceil(center+radius-0.5), srcN-1)
//     GPU 用 **float** 算同一条式子时，**整数倍缩放**会让边界参数恰好落在整数上
//     （center=2i+0.5、radius=1.0 ⇒ 两个边界参数都恰是整数），任何 1 ulp 的差异都会
//     把抽头范围多算/少算一个：
//       · box：每个抽头权重都是 1 ⇒ 结果直接变（真机 2.65/18.72%，max127）
//       · anti-ringing：min/max 邻域变 ⇒ 个别像素被钳到不同值（真机 0.05%，max31/53）
//       · 非 AR 的 Lanczos/Mitchell：边界处权重恰好 0 ⇒ 看不出来（真机通过）
//     ⇒ 边界一律由 CPU 用 double 算好（与 buildTaps 同式），shader 只 texelFetch。
//   `startOut` / `count` = 要算的是**完整输出**里 [startOut, startOut+count) 这段（= 可见窗口），
//   这样纹理尺寸只跟**一屏**有关（放大到 8× 时 full 可能有几万像素，超过最大纹理尺寸）。
//   返回 count × RGBA32F（每个可见输出索引一个 texel：(lo, hi, 0, 0)）。
QByteArray tapBoundsBytes(int srcN, int fullN, int startOut, int count,
                          const scaler::Params &p, bool nearest);

// ── QRhi 三段管线 ─────────────────────────────────────────────────────
class Pipeline
{
public:
    explicit Pipeline(QRhi *rhi);
    ~Pipeline();

    bool isValid() const { return m_err.isEmpty(); }   // 构造期致命错误
    QString error() const { return m_err.isEmpty() ? m_lastErr : m_err; }

    // 源图（隐式共享；真正上传发生在下一次 render）
    void setSourceImage(const QImage &img);
    QImage sourceImage() const { return m_src; }
    QSize sourceSize() const { return m_srcSize; }

    // finalRT == nullptr → 画进内部输出纹理（离屏对照用，可从 outputTexture() 回读）
    // finalRT != nullptr → 最后一趟直接画进它（尺寸必须 == plan.dst；QQuickRhiItem 走这条）
    // readback != nullptr 时，最后一趟的 endPass 里顺带把输出纹理读回（Qt 文档的离屏示例同款做法）
    bool render(QRhiCommandBuffer *cb, QRhiResourceUpdateBatch *rub, const Plan &plan,
                QRhiRenderTarget *finalRT = nullptr, QRhiReadbackResult *readback = nullptr);

    QRhiTexture *outputTexture() const { return m_outTex; }
    QRhiRenderTarget *outputRenderTarget() const { return m_rtOut; }
    QSize outputSize() const { return m_outSize; }

    // 诊断：本次 render 实际做了哪些趟（日志/自检用）
    QString lastPassLog() const { return m_passLog; }

    // 中间纹理（诊断/显存核算用）
    QSize linearTextureSize() const { return m_linSize; }
    QSize hTextureSize() const { return m_hSize; }
    bool  hArActive() const { return m_hAr; }

private:
    bool ensureSource(QRhiResourceUpdateBatch *rub);
    bool ensureTargets(const Plan &plan, bool needOwnOut);
    bool ensureBindings(bool useLin, QRhiRenderPassDescriptor *finalRp);
    bool ensurePipelines(QRhiRenderPassDescriptor *finalRp);
    // 最后一趟（→ RGBA8）的管线必须与**调用方给的渲染目标**附件结构一致
    //   （QQuickRhiItem 的自动 RT 带 depth-stencil，离屏自检的不带；GL 的
    //    isCompatible 恒真，但 Vulkan/D3D/Metal 会按 serializedFormat 真比对）
    bool ensureFinalPipelines(QRhiRenderPassDescriptor *rp);
    void invalidateSrbs();   // 只标脏：SRB 对象被管线持有，绝不销毁换对象
    void destroyBounds();
    void destroyLin();
    void destroyH();
    void destroyOut();
    void releaseAll();

    QRhi *m_rhi = nullptr;
    QString m_err;        // 致命（构造/资源创建）
    QString m_lastErr;    // 最近一次 render 的错误（每次 render 清空）
    QString m_passLog;
    float m_uboScratch[16] = {};

    // shader（qsb 烘焙产物，编译期进资源）与管线
    QShader m_vs, m_fsLin, m_fsRes, m_fsArH, m_fsArV;
    QRhiGraphicsPipeline *m_pLin = nullptr;    // → RGBA16F ×1
    QRhiGraphicsPipeline *m_pResH = nullptr;   // → RGBA16F ×1
    QRhiGraphicsPipeline *m_pResV = nullptr;   // → RGBA8   ×1
    QRhiGraphicsPipeline *m_pArH = nullptr;    // → RGBA16F ×3
    QRhiGraphicsPipeline *m_pArV = nullptr;    // → RGBA8   ×1
    QRhiSampler *m_sampler = nullptr;
    QRhiBuffer *m_vbuf = nullptr;
    bool m_vbufPending = true;
    QRhiBuffer *m_uboLin = nullptr;
    QRhiBuffer *m_uboResH = nullptr;
    QRhiBuffer *m_uboResV = nullptr;
    QRhiBuffer *m_uboArH = nullptr;
    QRhiBuffer *m_uboArV = nullptr;
    QRhiShaderResourceBindings *m_srbLin = nullptr;
    QRhiShaderResourceBindings *m_srbResH = nullptr;
    QRhiShaderResourceBindings *m_srbResV = nullptr;
    QRhiShaderResourceBindings *m_srbArH = nullptr;
    QRhiShaderResourceBindings *m_srbArV = nullptr;
    bool m_srbDirty = true;
    bool m_srbUseLin = true;      // H 趟的输入纹理：true = 线性化纹理，false = 源纹理（nearest 通路）

    // 管线模板（1×1 的三种附件组合）：管线一旦建好就与具体尺寸无关
    QRhiTexture *m_tpl16 = nullptr;
    QRhiTexture *m_tpl16b = nullptr;
    QRhiTexture *m_tpl16c = nullptr;
    QRhiTexture *m_tpl8 = nullptr;
    QRhiTextureRenderTarget *m_tplRt16 = nullptr;
    QRhiTextureRenderTarget *m_tplRt3 = nullptr;
    QRhiTextureRenderTarget *m_tplRt8 = nullptr;
    QRhiRenderPassDescriptor *m_finalRp = nullptr;
    QRhiRenderPassDescriptor *m_tplRp16 = nullptr;
    QRhiRenderPassDescriptor *m_tplRp3 = nullptr;
    QRhiRenderPassDescriptor *m_tplRp8 = nullptr;

    // 纹理 / 渲染目标（尺寸驱动，尺寸不变就复用）
    QRhiTexture *m_srcTex = nullptr;
    QSize m_srcTexSize;
    QImage m_src;
    QSize m_srcSize;
    quint64 m_srcGen = 0;
    quint64 m_uploadedGen = ~0ull;

    QRhiTexture *m_linTex = nullptr;
    QRhiTextureRenderTarget *m_rtLin = nullptr;
    QSize m_linSize;

    QRhiTexture *m_hTex = nullptr;
    QRhiTexture *m_hMinTex = nullptr;
    QRhiTexture *m_hMaxTex = nullptr;
    QRhiTextureRenderTarget *m_rtH = nullptr;
    QRhiTextureRenderTarget *m_rtHAr = nullptr;
    QSize m_hSize;
    bool m_hAr = false;

    QRhiTexture *m_outTex = nullptr;
    QRhiTextureRenderTarget *m_rtOut = nullptr;
    QSize m_outSize;

    // 抽头边界纹理（H 轴 full.w 个 texel、V 轴 full.h 个；只在 src/full/核 变化时重建）
    struct BoundsKey {
        QSize src, full, dst;
        int offX = 0, offY = 0;
        int filterIdx = 0, lobes = 3;
        bool nearest = false;
        bool operator==(const BoundsKey &o) const
        {
            return src == o.src && full == o.full && dst == o.dst && offX == o.offX && offY == o.offY
                   && filterIdx == o.filterIdx && lobes == o.lobes && nearest == o.nearest;
        }
    };
    QRhiTexture *m_boundsH = nullptr;
    QRhiTexture *m_boundsV = nullptr;
    BoundsKey m_boundsKey;
    bool m_boundsValid = false;
    bool m_boundsUploaded = false;
};

} // namespace gpuscale

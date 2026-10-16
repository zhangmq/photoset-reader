#version 440

// ②③ 分离式重采样（水平 / 垂直共用本 shader；由 p0.y(axis) 选轴）
//
// 与 gpubench.cpp kFragLanczosBody **逐字对应**，只多了一个 uOutOffset（p2.w）：
//   本组件的 pan 用「输出空间偏移」表达（可见窗口起点），uOutOffset = 0 时
//   公式与 gpubench 完全一致（等价性对照全部用 0）。
//
// ⚠️ 已知坑（来自 12 号调研，勿再踩）：
//   · 沿轴的那个维度必须由调用方按轴取（水平趟 outW、垂直趟 outH）→ 这里用 p0.z/p0.w 传进来
//   · linear→sRGB 必须 clamp(c,0,1)：Lanczos 会振铃到负值，pow(负数,1/2.4)=NaN
//   · 核宽必须用 fscale = max(1, scale)：放大（scale<1）时不夹会把核拉宽，权重与 CPU 不同
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 p0;   // x=filterIdx(0=lanczos 1=box 2=mitchell 3=nearest) y=axis(0=h 1=v) z=outN w=inN
    vec4 p1;   // x=fscale y=radius z=encode(1=linear→sRGB) w=lobes
    vec4 p2;   // x=mitchellB y=mitchellC z=antiring w=outOffset（输出空间起点）
    vec4 p3;   // x=flipY（GL 需要把垂直趟的索引翻过来）y=axisSize（沿轴的目标像素数）
};

layout(binding = 1) uniform sampler2D uIn;
layout(binding = 2) uniform sampler2D uBounds;   // 每个输出索引一个 texel：(lo, hi, -, -)，由 **CPU** 用 double 算好

const float kPi = 3.14159265358979323846;

vec3 l2s(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

float sinc_(float x)
{
    return x == 0.0 ? 1.0 : sin(kPi * x) / (kPi * x);
}

float lanczosN(float x, int lobes)
{
    float a = float(lobes);
    return (x <= -a || x >= a) ? 0.0 : sinc_(x) * sinc_(x / a);
}

float mitchellBC(float x, float B, float C)
{
    float a = abs(x);
    if (a < 1.0)
        return ((12.0 - 9.0 * B - 6.0 * C) * a * a * a
                + (-18.0 + 12.0 * B + 6.0 * C) * a * a
                + (6.0 - 2.0 * B)) / 6.0;
    if (a < 2.0)
        return ((-B - 6.0 * C) * a * a * a
                + (6.0 * B + 30.0 * C) * a * a
                + (-12.0 * B - 48.0 * C) * a
                + (8.0 * B + 24.0 * C)) / 6.0;
    return 0.0;
}

void main()
{
    int   flt = int(p0.x + 0.5);
    int   axis   = int(p0.y + 0.5);
    int   outN   = int(p0.z + 0.5);
    int   inN    = int(p0.w + 0.5);
    float fscale = p1.x;
    float radius = p1.y;
    int   encode = int(p1.z + 0.5);
    int   lobes  = int(p1.w + 0.5);
    float B      = p2.x;
    float C      = p2.y;
    float off    = p2.w;
    float flipY  = p3.x;
    float axisSize = p3.y;

    // 沿轴的输出索引（+ 可见窗口起点 off）。
    //   ★ GL（isYUpInFramebuffer）上 QQuickRhiItem 合成时会镜像 UV，所以垂直趟要把索引翻过来，
    //     否则画面上下颠倒；Vulkan/D3D/Metal 不翻。flipY 由调用方按后端给（见 gpuscale.h）。
    //   flipY == 0 时与 gpubench.cpp 的公式**逐字相同**。
    float coord = (axis == 0 ? gl_FragCoord.x : gl_FragCoord.y) - 0.5;
    float r = (flipY > 0.5 && axis == 1) ? (axisSize - 1.0 - coord) : coord;
    float i = r + off;
    float scale = float(inN) / float(outN);
    float center = (i + 0.5) * scale - 0.5;
    if (flt == 3)                       // 最近邻（像素完美通路）：唯一的抽头取四舍五入后的整像素
        center = floor(center + 0.5);

    // ★ 抽头范围**不由 GPU 判定**：整数倍缩放下 center/radius 会让边界参数恰好落在整数上，
    //   float 与 CPU 的 double 差 1 ulp 就会多/少一个抽头（真机 NVIDIA 实测：box 整幅偏、
    //   anti-ringing 个别像素被钳错）。边界一律读 CPU 算好的纹理（见 gpuscale.h 的说明）。
    vec4 b = texelFetch(uBounds, ivec2(int(r + 0.5), 0), 0);
    int lo = int(b.x + 0.5);
    int hi = int(b.y + 0.5);
    lo = clamp(lo, 0, inN - 1);          // CPU 已夹过；这里只防纹理/格式意外的越界
    hi = clamp(hi, 0, inN - 1);
    if (hi < lo) hi = lo;

    vec3 acc = vec3(0.0);
    float sum = 0.0;
    for (int k = lo; k <= hi; ++k) {
        float w;
        if (flt == 3) {
            w = 1.0;
        } else {
            float x = (float(k) - center) / fscale;
            w = (flt == 2) ? mitchellBC(x, B, C)
                              : ((flt == 1) ? 1.0 : lanczosN(x, lobes));
        }
        vec2 uv = vUv;
        if (axis == 0) uv.x = (float(k) + 0.5) / float(inN);
        else           uv.y = (float(k) + 0.5) / float(inN);
        acc += texture(uIn, uv).rgb * w;
        sum += w;
    }

    vec3 c = acc / max(sum, 1e-6);
    if (encode == 1)
        c = l2s(c);
    fragColor = vec4(c, 1.0);
}

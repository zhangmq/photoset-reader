#version 440

// anti-ringing 的**水平趟**：输出 color + 源邻域 min/max（三颜色附件 MRT）
//   与 gpubench.cpp kFragArHBody 逐字一致（只多了 uOutOffset）。
//   min/max 取的是**本趟抽头覆盖到的源像素**在三个通道上的极值（线性空间）。
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 outMin;
layout(location = 2) out vec4 outMax;

layout(std140, binding = 0) uniform buf {
    vec4 p0;   // x=filterIdx y=axis z=outN w=inN
    vec4 p1;   // x=fscale y=radius z=encode(未用) w=lobes
    vec4 p2;   // x=mitchellB y=mitchellC z=antiring(未用) w=outOffset
    vec4 p3;   // x=flipY（本趟用不到：水平趟不翻）y=axisSize
};

layout(binding = 1) uniform sampler2D uIn;
layout(binding = 2) uniform sampler2D uBounds;   // (lo, hi)，CPU 用 double 算好

const float kPi = 3.14159265358979323846;

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

    // ★ 抽头范围读 CPU 算好的边界（理由见 gpuscale.h：整数边界上 float/double 会差一个抽头，
    //   而 anti-ringing 的 min/max 邻域正是由抽头范围决定的）
    vec4 b = texelFetch(uBounds, ivec2(int(r + 0.5), 0), 0);
    int k0 = clamp(int(b.x + 0.5), 0, inN - 1);
    int k1 = clamp(int(b.y + 0.5), 0, inN - 1);
    if (k1 < k0) k1 = k0;

    vec3 acc = vec3(0.0);
    float sum = 0.0;
    vec3 vmin = vec3(1e30);
    vec3 vmax = vec3(-1e30);
    for (int k = k0; k <= k1; ++k) {
        float x = (float(k) - center) / fscale;
        float w = (flt == 2) ? mitchellBC(x, B, C)
                                : ((flt == 1) ? 1.0 : lanczosN(x, lobes));
        vec2 uv = vUv;
        if (axis == 0) uv.x = (float(k) + 0.5) / float(inN);
        else           uv.y = (float(k) + 0.5) / float(inN);
        vec3 v = texture(uIn, uv).rgb;
        acc += v * w;
        sum += w;
        vmin = min(vmin, v);
        vmax = max(vmax, v);
    }

    fragColor = vec4(acc / max(sum, 1e-6), 1.0);
    outMin = vec4(vmin, 1.0);
    outMax = vec4(vmax, 1.0);
}

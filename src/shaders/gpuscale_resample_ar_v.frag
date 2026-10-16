#version 440

// anti-ringing 的**垂直趟**：把结果夹到水平趟传下来的 min/max（抑制 Lanczos 负瓣振铃），再回 sRGB
//   与 gpubench.cpp kFragArVBody 逐字一致，并补上 antiring 强度（与 CPU `v + (clamped-v)*antiring` 同式：
//   mix(v, clamp(v,lo,hi), antiring) ≡ v + (clamped - v) * antiring）。
//   antiring = 0 时与 CPU 的 `wantAntiRing == false` 路径等价（结果不被夹）。
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 p0;   // x=filterIdx y=axis z=outN w=inN
    vec4 p1;   // x=fscale y=radius z=encode(1=linear→sRGB) w=lobes
    vec4 p2;   // x=mitchellB y=mitchellC z=antiring w=outOffset（输出空间起点）
    vec4 p3;   // x=flipY（GL 需要把垂直趟的索引翻过来）y=axisSize（沿轴的目标像素数）
};

layout(binding = 1) uniform sampler2D uIn;
layout(binding = 2) uniform sampler2D uBounds;   // (lo, hi)，CPU 用 double 算好
layout(binding = 3) uniform sampler2D uMin;
layout(binding = 4) uniform sampler2D uMax;

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
    int   flt     = int(p0.x + 0.5);
    int   axis    = int(p0.y + 0.5);
    int   outN    = int(p0.z + 0.5);
    int   inN     = int(p0.w + 0.5);
    float fscale  = p1.x;
    float radius  = p1.y;
    int   encode  = int(p1.z + 0.5);
    int   lobes   = int(p1.w + 0.5);
    float B       = p2.x;
    float C       = p2.y;
    float antiring = p2.z;
    float off     = p2.w;
    float flipY   = p3.x;
    float axisSize = p3.y;

    // 沿轴的输出索引（+ 可见窗口起点 off）；★ GL 上垂直趟必须翻转，理由见 gpuscale.h / resample.frag
    float coord = (axis == 0 ? gl_FragCoord.x : gl_FragCoord.y) - 0.5;
    float r = (flipY > 0.5 && axis == 1) ? (axisSize - 1.0 - coord) : coord;
    float i = r + off;
    float scale = float(inN) / float(outN);
    float center = (i + 0.5) * scale - 0.5;

    // ★ 抽头范围读 CPU 算好的边界（与 AR-H 同一份纹理，理由见 gpuscale.h）
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
        acc += texture(uIn, uv).rgb * w;
        sum += w;
        vmin = min(vmin, texture(uMin, uv).rgb);
        vmax = max(vmax, texture(uMax, uv).rgb);
    }

    vec3 c = acc / max(sum, 1e-6);
    c = mix(c, clamp(c, vmin, vmax), antiring);
    if (encode == 1)
        c = l2s(c);
    fragColor = vec4(c, 1.0);
}

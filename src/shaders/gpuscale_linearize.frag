#version 440

// ① 线性化：sRGB8 → 线性 float
//   与 gpubench.cpp kFragLinBody 逐字一致（含 s2l 公式）。
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 p0;   // x=filterIdx y=axis z=outN w=inN        （本趟未用，占位以保持块布局一致）
    vec4 p1;   // x=fscale y=radius z=encode w=lobes
    vec4 p2;   // x=mitchellB y=mitchellC z=antiring w=outOffset
};

layout(binding = 1) uniform sampler2D uIn;

vec3 s2l(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main()
{
    fragColor = vec4(s2l(texture(uIn, vUv).rgb), 1.0);
}

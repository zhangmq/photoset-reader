#version 440

// 全屏四边形（triangle strip，4 顶点）——顶点数据由 GPUScalePipeline 提供。
// 顶点顺序与 gpubench.cpp 的 quad[] 一致：(-1,-1) (1,-1) (-1,1) (1,1)
//   ⇒ vUv 与 gl_FragCoord 的关系 = gl_FragCoord.xy == vUv * outSize
//   （shader 数学与 gpubench.cpp 逐字对应，见 src/gpubench.cpp kVertBody）
layout(location = 0) in vec2 aPos;
layout(location = 0) out vec2 vUv;

void main()
{
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}

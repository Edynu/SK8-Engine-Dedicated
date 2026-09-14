// Lua dev console overlay (F7): draws the CEF off-screen-rendered browser
// texture as a full-screen triangle, clipped to the console's fixed
// on-screen rect by the viewport (see DrawDevConsoleOverlay in
// skate3_native_scene_gpu.cpp - no vertex buffer, matching blur.hlsl's
// vs_main convention).
Texture2D<float4> src : register(t0);
SamplerState smp_clamp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = uv;
  return o;
}
// CEF's OSR paint buffer is BGRA32; this project's texture format is
// R8G8B8A8_UNORM, so a straight upload lands each pixel's bytes swapped
// (what the shader reads as .r is CEF's blue channel, and vice versa) -
// swizzle it back here rather than shuffling bytes on the CPU every paint.
float4 ps_main(VSOut i) : SV_Target {
  float4 c = src.SampleLevel(smp_clamp, i.uv, 0);
  return c.bgra;
}

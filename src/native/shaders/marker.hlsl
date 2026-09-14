// In-world marker/nameplate billboards.
//
// Quads are built CPU-side (skate3_native_scene_gpu.cpp, DrawWorldMarkers):
// the corner offsets are already applied in WORLD space against a camera-facing
// basis, so the VS here only projects, exactly like spline.hlsl. Doing the
// billboarding on the CPU rather than in the VS keeps this shader free of a
// camera basis it would otherwise need passed in, and there are only ever a
// handful of markers.
//
// NO TEXTURE. The marker ring is drawn procedurally from the quad's own UV,
// which removes an asset, an upload and a sampler from the whole feature.
// Retail's own marker icons were the alternative and are not reachable: they
// are APT (Flash) assets packed in .rx2, resolved by name through the UI layer,
// not SimpleDraw quads we could bind.
cbuffer C : register(b0) {
  float4 vp0;  // scene view_proj rows (row-vector: clip = x*vp0+y*vp1+z*vp2+w*vp3)
  float4 vp1;
  float4 vp2;
  float4 vp3;
  float4 tint;    // straight-alpha RGBA for this marker
  float4 params;  // x = hold progress 0..1, y = 1 when this is the active
                  // marker, zw = unused
};

// Bound only by the text variant; the ring variant is entirely procedural and
// binds nothing.
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);

struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

VSOut vs_main(float4 p : POSITION, float2 uv : TEXCOORD0) {
  VSOut o;
  o.pos = p.x * vp0 + p.y * vp1 + p.z * vp2 + vp3;
  o.uv = uv;
  return o;
}

// HDR=1: the scene target holds the tone chain's pre-tonemap input (scene.hlsl
// ToneOut), so gamma-authored colours have to encode through the chain's exact
// inverse here or they come out of the host tonemap wrong. Same transform
// spline.hlsl uses - see SplineOut there for the derivation.
float3 MarkerOut(float3 c) {
#ifdef HDR
  float3 tm = c * c * (2.0 / (1.41 * 1.41));
  float3 lo = 1.0 - sqrt(saturate(1.0 - tm));
  float3 hi = 4.0 * tm - 3.0;
  return lerp(lo, hi, step(1.0, tm));
#else
  return c;
#endif
}

float4 ps_main(VSOut i) : SV_Target {
  // Centred coordinates: -1..1 across the quad.
  float2 d = i.uv * 2.0 - 1.0;
  float r = length(d);

  // A ring, not a disc, so the marker frames the spot the player stands on
  // instead of hiding it. fwidth gives a constant-width antialiased edge at
  // any distance, which matters because these are world-space and so shrink
  // with range - a fixed epsilon would alias badly far away.
  const float kOuter = 1.0;
  const float kInner = 0.62;
  float aa = max(fwidth(r), 1e-5);
  float ring = smoothstep(kOuter, kOuter - aa * 2.0, r) *
               smoothstep(kInner - aa * 2.0, kInner, r);

  // Hold progress sweeps clockwise from the top as a filled arc, so the
  // player can see the hold registering on the marker itself rather than only
  // in a separate prompt.
  float progress = saturate(params.x);
  if (progress > 0.0) {
    // atan2 in screen-ish quad space: 0 at the top, increasing clockwise.
    float angle = atan2(d.x, -d.y);          // -pi..pi, 0 at top
    float turn = (angle < 0.0 ? angle + 6.28318530718 : angle) / 6.28318530718;
    // The swept part reads brighter; the rest stays the base ring.
    ring *= (turn <= progress) ? 1.0 : 0.45;
  }

  if (ring <= 0.001) {
    clip(-1.0);
  }

  // The active marker pulses slightly brighter so which one is selected is
  // unambiguous when several are in view.
  float gain = lerp(1.0, 1.6, saturate(params.y));
  float3 rgb = tint.rgb * gain;
  return float4(MarkerOut(rgb), ring * tint.a);
}

// Text billboards (player nameplates). The glyph bitmap is rasterised on the
// CPU once per distinct string (skate3_text_texture.h) with its dark outline
// already baked into the alpha, so this only has to sample and tint - no
// second shadow pass, no glyph atlas arithmetic.
float4 ps_text(VSOut i) : SV_Target {
  float4 c = tex.Sample(smp, i.uv);
  if (c.a <= 0.003) {
    clip(-1.0);
  }
  return float4(MarkerOut(c.rgb * tint.rgb), c.a * tint.a);
}

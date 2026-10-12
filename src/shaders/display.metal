// Accumulated radiance -> 8-bit image (AgX tonemapping), and the blit to screen.

#include "shared.h"

// AgX by Troy Sobotka, using Benjamin Wrensch's polynomial fit. Output is ~sRGB.
float3 agx(float3 c) {
  const float3x3 inset  = float3x3(float3(0.842479f, 0.0423282f, 0.0423757f), float3(0.0784336f, 0.878469f, 0.0784336f),
       float3(0.0792237f, 0.0791661f, 0.879143f));
  const float3x3 outset = float3x3(float3(1.196879f, -0.052897f, -0.052972f), float3(-0.098021f, 1.151903f, -0.098043f),
      float3(-0.099030f, -0.098961f, 1.151074f));
  const float    lo = -12.47393f, hi = 4.026069f;

  float3 x  = (clamp(log2(inset * max(c, float3(1e-10f))), lo, hi) - lo) / (hi - lo);
  float3 x2 = x * x, x4 = x2 * x2;
  x = 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;

  // A touch more contrast and saturation than the base look.
  x          = pow(max(x, float3(0.0f)), float3(1.08f));
  float luma = dot(x, float3(0.2126f, 0.7152f, 0.0722f));
  return saturate(outset * (luma + 1.12f * (x - luma)));
}

kernel void resolve(device const float4* accumulation [[buffer(0)]], constant Uniforms& U [[buffer(1)]],
    texture2d<float, access::write> image [[texture(0)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= U.resolution.x || gid.y >= U.resolution.y) return;
  float4 sum   = accumulation[gid.y * U.resolution.x + gid.x];
  float3 color = agx(sum.rgb / max(sum.a, 1.0f) * exp2(U.exposure));
  float  noise = fract(52.9829189f * fract(dot(float2(gid), float2(0.06711056f, 0.00583715f))));
  image.write(float4(color + (noise - 0.5f) / 255.0f, 1.0f), gid);  // dither away banding
}

struct Blit {
  float4 position [[position]];
  float2 uv;
};

vertex Blit blitVertex(uint id [[vertex_id]]) {
  float2 uv = float2((id << 1) & 2, id & 2);  // one triangle covering the screen
  return {float4(uv * float2(2, -2) + float2(-1, 1), 0, 1), uv};
}

fragment float4 blitFragment(Blit in [[stage_in]], texture2d<float> image [[texture(0)]]) {
  constexpr sampler linear(filter::linear);
  return image.sample(linear, in.uv);
}

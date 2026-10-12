// Structs shared byte-for-byte between the C++ host and the Metal shaders.
#ifndef SHARED_H
#define SHARED_H

#ifdef __METAL_VERSION__
#include <metal_stdlib>
using namespace metal;
typedef packed_float3 packed3;
#else
#include <simd/simd.h>

#include <cstdint>
using simd::float3;
using simd::float4;
using simd::uint2;
using uint = uint32_t;

struct packed3 {
  float x = 0, y = 0, z = 0;
  packed3() = default;
  packed3(float3 v) : x(v.x), y(v.y), z(v.z) {}
  operator float3() const { return {x, y, z}; }
};
#endif

enum : uint { kAlbedoTableSize = 32 };

enum MaterialType : uint { MaterialDiffuse, MaterialPlastic, MaterialMetal, MaterialGlass, MaterialLight };

// Inner nodes have count == 0 and children at `first` and `first + 1`.
// Leaves hold `count` triangles starting at `first`.
struct BvhNode {
  packed3 min;
  uint    first;
  packed3 max;
  uint    count;
};

struct Triangle {
  float4 v0, e1, e2;  // vertex and edges, as Möller–Trumbore wants them
};

struct TriangleShading {
  packed3 n0, n1, n2;
  uint    material;
};

struct Material {
  packed3 albedo;  // F0 for metals, tint for glass
  float   roughness;
  packed3 emission;
  float   ior;
  packed3 absorption;  // inside glass, per unit length
  uint    type;
};

struct LightRef {
  uint  triangle;
  float cdf;  // lights are picked proportionally to power
};

struct Uniforms {
  float3 cameraPosition, cameraRight, cameraUp, cameraForward;  // right/up scaled by tan(fov/2)
  float3 skyZenith, skyHorizon, skyGround;
  uint2  resolution;
  uint   sampleIndex;
  uint   maxBounces;
  float  aperture;
  float  focusDistance;
  uint   blades;  // 0 = round aperture
  uint   lightCount;
  float  lightPower;  // Σ area · luminance(emission)
  float  clamp;       // max indirect radiance per sample
  float  exposure;
  uint   hideLights;  // lights invisible to camera rays
};

#ifndef __METAL_VERSION__
static_assert(
    sizeof(BvhNode) == 32 && sizeof(Triangle) == 48 && sizeof(TriangleShading) == 40 && sizeof(Material) == 48);
#endif

#endif

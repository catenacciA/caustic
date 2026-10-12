#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "shaders/shared.h"

// Note: float3{v} would only set .x.
inline float3 splat(float v) { return float3{v, v, v}; }

struct Mesh {
  std::vector<float3>   positions, normals;
  std::vector<uint32_t> indices;

  uint32_t vertex(float3 p, float3 n);
  void     triangle(uint32_t a, uint32_t b, uint32_t c);  // wound to match the normals; slivers dropped
  void     append(const Mesh& other);
  Mesh&    transform(const simd::float4x4& m);
};

namespace shapes {
Mesh sphere(float3 center, float radius);
Mesh torus(float3 center, float radius, float thickness, simd::float4x4 orientation = matrix_identity_float4x4);
Mesh trefoil(float3 center, float scale, float thickness);
Mesh quad(float3 center, float3 halfU, float3 halfV);  // faces cross(halfU, halfV)
Mesh box(float3 lo, float3 hi);
Mesh panel(float3 center, float3 facing, float width, float height);
Mesh cyclorama();  // studio floor that curves up into the back wall
}  // namespace shapes

std::optional<Mesh> loadObj(const std::string& path, std::string& error);

simd::float4x4 translate(float3 t);
simd::float4x4 rotateX(float radians);
simd::float4x4 rotateY(float radians);
simd::float4x4 rotateZ(float radians);

Material diffuse(float3 albedo);
Material plastic(float3 albedo, float roughness);
Material metal(float3 f0, float roughness);
Material glass(float roughness, float3 tint = splat(1.f));  // tint = colour after 1 unit inside
Material light(float3 color, float strength);

struct Camera {
  float3 target        = splat(0.f);
  float  yaw           = 0.f;  // around +y; 0 looks down -z
  float  pitch         = 0.2f;
  float  distance      = 6.f;
  float  fov           = 35.f;  // vertical, degrees
  float  aperture      = 0.f;   // lens radius
  float  focusDistance = 6.f;
  uint   blades        = 6;

  float3 position() const;
  float3 forward() const { return simd::normalize(target - position()); }
};

class Scene {
 public:
  std::string name;
  Camera      camera;
  float3      skyZenith = splat(0.f), skyHorizon = splat(0.f), skyGround = splat(0.f);
  float       exposure   = 0.f;
  bool        hideLights = false;

  void                 add(const Mesh& mesh, const Material& material);
  void                 build();  // BVH + GPU arrays
  std::optional<float> intersect(float3 origin, float3 dir) const;

  // Filled by build(), uploaded as-is.
  std::vector<BvhNode>         nodes;
  std::vector<Triangle>        triangles;
  std::vector<TriangleShading> shading;
  std::vector<Material>        materials;
  std::vector<LightRef>        lights;
  float                        lightPower = 0.f;

 private:
  struct Pending {
    float3   p[3], n[3];
    uint32_t material;
  };
  std::vector<Pending> _pending;
};

extern const char* const kSceneNames[4];
std::unique_ptr<Scene>   makeScene(const std::string& name, const std::string& objPath = "");

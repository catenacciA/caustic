#include "scene.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <sstream>
#include <unordered_map>

using simd::float4x4;

// --- Meshes -------------------------------------------------------------------

uint32_t Mesh::vertex(float3 p, float3 n) {
  positions.push_back(p);
  normals.push_back(simd::normalize(n));
  return uint32_t(positions.size() - 1);
}

void Mesh::triangle(uint32_t a, uint32_t b, uint32_t c) {
  float3 e1 = positions[b] - positions[a], e2 = positions[c] - positions[a], n = simd::cross(e1, e2);
  if (simd::length_squared(n) <= 1e-12f * simd::length_squared(e1) * simd::length_squared(e2)) return;
  if (simd::dot(n, normals[a] + normals[b] + normals[c]) < 0) std::swap(b, c);
  indices.insert(indices.end(), {a, b, c});
}

void Mesh::append(const Mesh& other) {
  auto base = uint32_t(positions.size());
  positions.insert(positions.end(), other.positions.begin(), other.positions.end());
  normals.insert(normals.end(), other.normals.begin(), other.normals.end());
  for (uint32_t i : other.indices) indices.push_back(base + i);
}

Mesh& Mesh::transform(const float4x4& m) {
  float4x4 normalMatrix = simd::transpose(simd::inverse(m));
  for (float3& p : positions) p = (m * simd::make_float4(p, 1)).xyz;
  for (float3& n : normals) n = simd::normalize((normalMatrix * simd::make_float4(n, 0)).xyz);
  return *this;
}

float4x4 translate(float3 t) {
  return float4x4{
      simd::float4{1, 0, 0, 0}, simd::float4{0, 1, 0, 0}, simd::float4{0, 0, 1, 0}, simd::make_float4(t, 1)};
}
float4x4 rotateX(float a) { return simd_matrix4x4(simd_quaternion(a, float3{1, 0, 0})); }
float4x4 rotateY(float a) { return simd_matrix4x4(simd_quaternion(a, float3{0, 1, 0})); }
float4x4 rotateZ(float a) { return simd_matrix4x4(simd_quaternion(a, float3{0, 0, 1})); }

namespace shapes {

// Tessellates f(u, v) -> {position, normal} over [0,1]², sharing seam vertices
// along wrapped directions so normals stay smooth.
template <typename F>
Mesh grid(int nu, int nv, bool wrapU, bool wrapV, F&& f) {
  Mesh m;
  int  cu = wrapU ? nu : nu + 1, cv = wrapV ? nv : nv + 1;
  for (int j = 0; j < cv; ++j)
    for (int i = 0; i < cu; ++i) {
      auto [p, n] = f(double(i) / nu, double(j) / nv);
      m.vertex(p, n);
    }
  auto at = [&](int i, int j) { return uint32_t(j % cv * cu + i % cu); };
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      m.triangle(at(i, j), at(i + 1, j), at(i + 1, j + 1));
      m.triangle(at(i, j), at(i + 1, j + 1), at(i, j + 1));
    }
  return m;
}

float3 f3(double x, double y, double z) { return float3{float(x), float(y), float(z)}; }

Mesh sphere(float3 c, float r) {
  return grid(128, 64, true, false, [&](double u, double v) {
    float3 n = f3(
        std::sin(M_PI * v) * std::cos(2 * M_PI * u), std::cos(M_PI * v), std::sin(M_PI * v) * std::sin(2 * M_PI * u));
    return std::pair{c + r * n, n};
  });
}

Mesh torus(float3 c, float radius, float thickness, float4x4 orientation) {
  return grid(160, 48, true, true, [&](double u, double v) {
    float3 ring = f3(std::cos(2 * M_PI * u), 0, std::sin(2 * M_PI * u));
    float3 n    = ring * float(std::cos(2 * M_PI * v)) + float3{0, float(std::sin(2 * M_PI * v)), 0};
    return std::pair{radius * ring + thickness * n, n};
  }).transform(translate(c) * orientation);
}

// A tube swept along a trefoil knot. Parallel-transport frames keep the tube
// from twisting; the leftover twist after one loop is spread along its length.
Mesh trefoil(float3 c, float scale, float thickness) {
  const int           n = 600;
  std::vector<float3> point(n), tangent(n), normal(n);
  for (int i = 0; i < n; ++i) {
    double t   = 2 * M_PI * i / n;
    point[i]   = scale * f3(std::sin(t) + 2 * std::sin(2 * t), std::cos(t) - 2 * std::cos(2 * t), -std::sin(3 * t));
    tangent[i] = simd::normalize(
        f3(std::cos(t) + 4 * std::cos(2 * t), -std::sin(t) + 4 * std::sin(2 * t), -3 * std::cos(3 * t)));
  }
  auto transport = [](float3 v, float3 t) { return simd::normalize(v - simd::dot(v, t) * t); };
  normal[0]      = simd::normalize(simd::cross(tangent[0], float3{0, 0, 1}));
  for (int i = 1; i < n; ++i) normal[i] = transport(normal[i - 1], tangent[i]);

  float3 end   = transport(normal[n - 1], tangent[0]);
  float  twist = std::atan2(simd::dot(simd::cross(end, normal[0]), tangent[0]), simd::dot(end, normal[0]));
  for (int i = 0; i < n; ++i) {
    float a   = twist * i / n;
    normal[i] = normal[i] * std::cos(a) + simd::cross(tangent[i], normal[i]) * std::sin(a);
  }

  return grid(n, 48, true, true, [&](double u, double v) {
    int    i = int(std::lround(u * n)) % n;
    float3 d = normal[i] * float(std::cos(2 * M_PI * v)) +
               simd::cross(tangent[i], normal[i]) * float(std::sin(2 * M_PI * v));
    return std::pair{c + point[i] + thickness * d, d};
  });
}

Mesh quad(float3 c, float3 u, float3 v) {
  Mesh   m;
  float3 n = simd::cross(u, v);
  m.vertex(c - u - v, n), m.vertex(c + u - v, n), m.vertex(c + u + v, n), m.vertex(c - u + v, n);
  m.triangle(0, 1, 2);
  m.triangle(0, 2, 3);
  return m;
}

Mesh box(float3 lo, float3 hi) {
  float3 c = (lo + hi) / 2, h = (hi - lo) / 2, x{h.x, 0, 0}, y{0, h.y, 0}, z{0, 0, h.z};
  Mesh   m;
  for (float s : {1.f, -1.f}) {
    m.append(quad(c + s * x, s * y, z));
    m.append(quad(c + s * y, s * z, x));
    m.append(quad(c + s * z, s * x, y));
  }
  return m;
}

Mesh panel(float3 c, float3 facing, float width, float height) {
  float3 n    = simd::normalize(facing);
  float3 up   = std::abs(n.y) > 0.99f ? float3{0, 0, -1} : float3{0, 1, 0};
  float3 side = simd::normalize(simd::cross(up, n));
  return quad(c, side * width / 2, simd::cross(n, side) * height / 2);
}

Mesh cyclorama() {
  const float width = 40, front = 16, wall = -4, height = 14, bend = 4;
  // Profile in (z, y) with normals: floor, quarter-circle bend, back wall.
  std::vector<simd::float4> profile{simd::float4{front, 0, 0, 1}};
  for (int k = 0; k <= 48; ++k) {
    float a = float(M_PI / 2 * k / 48);
    profile.push_back(
        simd::float4{wall + bend - bend * std::sin(a), bend - bend * std::cos(a), std::sin(a), std::cos(a)});
  }
  profile.push_back(simd::float4{wall, height, 1, 0});

  Mesh m;
  for (auto p : profile)
    for (float x : {-width / 2, width / 2}) m.vertex(float3{x, p.y, p.x}, float3{0, p.w, p.z});
  for (uint32_t k = 0; k + 1 < profile.size(); ++k) {
    m.triangle(2 * k, 2 * k + 1, 2 * k + 3);
    m.triangle(2 * k, 2 * k + 3, 2 * k + 2);
  }
  return m;
}

}  // namespace shapes

std::optional<Mesh> loadObj(const std::string& path, std::string& error) {
  std::ifstream file(path);
  if (!file) return error = "cannot open " + path, std::nullopt;

  std::vector<float3>                           positions, normals;
  std::vector<std::vector<std::pair<int, int>>> faces;  // (position, normal) per corner
  auto resolve = [](int i, size_t count) { return i > 0 ? i - 1 : i < 0 ? int(count) + i : -1; };

  for (std::string line; std::getline(file, line);) {
    std::istringstream in(line);
    std::string        key;
    float              x = 0, y = 0, z = 0;
    in >> key;
    if (key == "v" && in >> x >> y >> z) positions.push_back(float3{x, y, z});
    if (key == "vn" && in >> x >> y >> z) normals.push_back(float3{x, y, z});
    if (key != "f") continue;
    auto& face = faces.emplace_back();
    for (std::string token; in >> token;) {
      int v = 0, t = 0, n = 0;
      if (std::sscanf(token.c_str(), "%d/%d/%d", &v, &t, &n) != 3 && std::sscanf(token.c_str(), "%d//%d", &v, &n) != 2)
        n = 0;
      int p = resolve(v, positions.size()), q = n ? resolve(n, normals.size()) : -1;
      if (p < 0 || p >= int(positions.size()) || q >= int(normals.size()))
        return error = "bad face in " + path, std::nullopt;
      face.push_back({p, q});
    }
    if (face.size() < 3) faces.pop_back();
  }

  // Area-weighted normals for corners without one.
  std::vector<float3> smooth(positions.size(), splat(0));
  for (auto& f : faces)
    for (size_t k = 1; k + 1 < f.size(); ++k) {
      float3 n = simd::cross(
          positions[f[k].first] - positions[f[0].first], positions[f[k + 1].first] - positions[f[0].first]);
      for (int i : {f[0].first, f[k].first, f[k + 1].first}) smooth[i] += n;
    }

  Mesh                                   mesh;
  std::unordered_map<uint64_t, uint32_t> ids;
  auto                                   corner = [&](std::pair<int, int> c) {
    auto [it, fresh] = ids.try_emplace(uint64_t(uint32_t(c.first)) << 32 | uint32_t(c.second), 0);
    if (fresh) {
      float3 n = c.second >= 0 ? normals[c.second] : smooth[c.first];
      it->second = mesh.vertex(positions[c.first], simd::length_squared(n) > 0 ? n : float3{0, 1, 0});
    }
    return it->second;
  };
  for (auto& f : faces)
    for (size_t k = 1; k + 1 < f.size(); ++k) mesh.triangle(corner(f[0]), corner(f[k]), corner(f[k + 1]));
  if (mesh.indices.empty()) return error = path + " has no faces", std::nullopt;
  return mesh;
}

// --- Materials and camera -----------------------------------------------------

Material diffuse(float3 albedo) { return {albedo, 1, splat(0), 1.5f, splat(0), MaterialDiffuse}; }
Material plastic(float3 albedo, float roughness) {
  return {albedo, roughness, splat(0), 1.5f, splat(0), MaterialPlastic};
}
Material metal(float3 f0, float roughness) { return {f0, roughness, splat(0), 1.5f, splat(0), MaterialMetal}; }
Material light(float3 color, float strength) { return {splat(0), 1, color * strength, 1.5f, splat(0), MaterialLight}; }

Material glass(float roughness, float3 tint) {
  float3 t = simd::clamp(tint, splat(1e-4f), splat(1));
  return {splat(1), roughness, splat(0), 1.5f, float3{-std::log(t.x), -std::log(t.y), -std::log(t.z)}, MaterialGlass};
}

float3 Camera::position() const {
  return target + distance * float3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
}

// --- Scene --------------------------------------------------------------------

void Scene::add(const Mesh& mesh, const Material& material) {
  materials.push_back(material);
  for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    Pending t{.material = uint32_t(materials.size() - 1)};
    for (int k = 0; k < 3; ++k)
      t.p[k] = mesh.positions[mesh.indices[i + k]], t.n[k] = mesh.normals[mesh.indices[i + k]];
    _pending.push_back(t);
  }
}

namespace {

struct Box {
  float3 lo = splat(FLT_MAX), hi = splat(-FLT_MAX);
  void   grow(float3 p) { lo = simd::min(lo, p), hi = simd::max(hi, p); }
  void   grow(const Box& b) { lo = simd::min(lo, b.lo), hi = simd::max(hi, b.hi); }
  float  area() const {
    float3 e = simd::max(hi - lo, splat(0));
    return e.x * e.y + e.y * e.z + e.z * e.x;
  }
};

// Binary BVH using the surface area heuristic, evaluated over 16 bins per axis.
std::vector<BvhNode> buildBvh(const std::vector<Box>& boxes, std::vector<uint32_t>& order) {
  constexpr int kBins = 16;
  order.resize(boxes.size());
  std::iota(order.begin(), order.end(), 0u);
  std::vector<float3> centers;
  for (auto& b : boxes) centers.push_back((b.lo + b.hi) / 2);

  std::vector<BvhNode> nodes;
  nodes.reserve(2 * boxes.size());
  auto push = [&](uint32_t first, uint32_t count) {
    Box b;
    for (uint32_t i = first; i < first + count; ++i) b.grow(boxes[order[i]]);
    nodes.push_back({b.lo, first, b.hi, count});
  };
  push(0, uint32_t(boxes.size()));

  for (std::vector<uint32_t> todo{0}; !todo.empty();) {
    uint32_t index = todo.back(), first = nodes[index].first, count = nodes[index].count;
    todo.pop_back();
    if (count <= 1) continue;

    Box centroids;
    for (uint32_t i = first; i < first + count; ++i) centroids.grow(centers[order[i]]);
    auto binOf = [&](uint32_t prim, int axis) {
      float lo = centroids.lo[axis], extent = centroids.hi[axis] - lo;
      return std::min(kBins - 1, int((centers[prim][axis] - lo) / extent * kBins));
    };

    int   bestAxis = -1, bestBin = 0;
    float bestCost = FLT_MAX;
    for (int axis = 0; axis < 3; ++axis) {
      if (centroids.hi[axis] - centroids.lo[axis] <= 1e-9f) continue;
      Box      bins[kBins], right;
      uint32_t counts[kBins] = {}, rightCount[kBins] = {};
      float    rightArea[kBins] = {};
      for (uint32_t i = first; i < first + count; ++i) {
        int b = binOf(order[i], axis);
        counts[b]++;
        bins[b].grow(boxes[order[i]]);
      }
      for (int k = kBins - 1, sum = 0; k > 0; --k) {
        right.grow(bins[k]);
        rightCount[k] = sum += counts[k];
        rightArea[k]  = right.area();
      }
      Box left;
      for (int k = 0, sum = 0; k < kBins - 1; ++k) {  // split between bin k and k + 1
        left.grow(bins[k]);
        sum += counts[k];
        float cost = sum * left.area() + rightCount[k + 1] * rightArea[k + 1];
        if (sum > 0 && rightCount[k + 1] > 0 && cost < bestCost) bestAxis = axis, bestBin = k, bestCost = cost;
      }
    }
    if (bestAxis < 0) continue;

    // Split only if it beats a leaf (traversal step ≈ one triangle test), but cap leaf size.
    float leafArea = Box{nodes[index].min, nodes[index].max}.area();
    if (1 + bestCost / std::max(leafArea, 1e-20f) >= count && count <= 8) continue;

    auto     begin = order.begin() + first;
    uint32_t mid   = uint32_t(
        std::partition(begin, begin + count, [&](uint32_t p) { return binOf(p, bestAxis) <= bestBin; }) - begin);
    if (mid == 0 || mid == count) continue;

    nodes[index].first = uint32_t(nodes.size());
    nodes[index].count = 0;
    push(first, mid);
    push(first + mid, count - mid);
    todo.push_back(nodes[index].first);
    todo.push_back(nodes[index].first + 1);
  }
  return nodes;
}

}  // namespace

void Scene::build() {
  std::vector<Box> boxes(_pending.size());
  for (size_t i = 0; i < _pending.size(); ++i)
    for (float3 p : _pending[i].p) boxes[i].grow(p);
  std::vector<uint32_t> order;
  nodes = buildBvh(boxes, order);

  // Lay triangles out in BVH leaf order and build the light-picking CDF.
  for (uint32_t i : order) {
    const Pending& t  = _pending[i];
    float3         e1 = t.p[1] - t.p[0], e2 = t.p[2] - t.p[0];
    triangles.push_back({simd::make_float4(t.p[0], 0), simd::make_float4(e1, 0), simd::make_float4(e2, 0)});
    shading.push_back({t.n[0], t.n[1], t.n[2], t.material});

    const Material& m     = materials[t.material];
    float           power = 0.5f * simd::length(simd::cross(e1, e2)) *
                  simd::dot(float3(m.emission), float3{0.2126f, 0.7152f, 0.0722f});
    if (m.type == MaterialLight && power > 0) lights.push_back({uint32_t(triangles.size() - 1), lightPower += power});
  }
  for (LightRef& l : lights) l.cdf /= lightPower;
  if (!lights.empty()) lights.back().cdf = 1;
  _pending.clear();
}

// Closest hit along a ray on the CPU (used for click-to-focus).
std::optional<float> Scene::intersect(float3 o, float3 d) const {
  float closest = FLT_MAX;
  for (int k = 0; k < 3; ++k) d[k] = std::abs(d[k]) < 1e-20f ? 1e-20f : d[k];  // avoid 0/0 in the slab test
  for (std::vector<uint32_t> todo{0}; !todo.empty();) {
    const BvhNode& n = nodes[todo.back()];
    todo.pop_back();
    float3 t0 = (float3(n.min) - o) / d, t1 = (float3(n.max) - o) / d;
    float3 near = simd::min(t0, t1), far = simd::max(t0, t1);
    if (std::max({near.x, near.y, near.z, 0.f}) > std::min({far.x, far.y, far.z, closest})) continue;
    if (n.count == 0) {
      todo.push_back(n.first), todo.push_back(n.first + 1);
      continue;
    }
    for (uint32_t i = n.first; i < n.first + n.count; ++i) {
      const Triangle& tri = triangles[i];
      float3          p = simd::cross(d, tri.e2.xyz), s = o - tri.v0.xyz, q = simd::cross(s, tri.e1.xyz);
      float           inv = 1 / simd::dot(tri.e1.xyz, p), u = simd::dot(s, p) * inv, v = simd::dot(d, q) * inv;
      float           t = simd::dot(tri.e2.xyz, q) * inv;
      if (u >= 0 && v >= 0 && u + v <= 1 && t > 1e-6f && t < closest) closest = t;
    }
  }
  return closest < FLT_MAX ? std::optional(closest) : std::nullopt;
}

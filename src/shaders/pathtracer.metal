// Unidirectional path tracer: BVH traversal, next-event estimation with MIS,
// GGX microfacet BSDFs and a thin-lens camera. Each dispatch adds one sample
// per pixel to the accumulation buffer (rgb = sum, a = count).

#include "shared.h"

constant float kPi    = 3.14159265f;
constant float kMiss  = 1e30f;  // fast-math assumes no infinities
constant float kSharp = 1e-3f;  // GGX alpha below which a lobe is a perfect mirror

// --- Random numbers -----------------------------------------------------------

uint pcg(uint v) {
  uint s = v * 747796405u + 2891336453u;
  uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}

float rand(thread uint& s) {
  s = pcg(s);
  return float(s >> 8) * 0x1p-24f;
}

float2 rand2(thread uint& s) {
  float x = rand(s);
  return float2(x, rand(s));
}

// --- Helpers ------------------------------------------------------------------

float luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }
float maxOf(float3 v) { return max(v.x, max(v.y, v.z)); }
float misWeight(float a, float b) { return a * a / max(a * a + b * b, 1e-30f); }

float3 clampTo(float3 c, float limit) {
  float m = maxOf(c);
  return limit > 0.0f && m > limit ? c * (limit / m) : c;
}

float3 nudge(float3 p, float3 dir) { return p + dir * 1e-4f * max(1.0f, maxOf(abs(p))); }

// Orthonormal basis around n (Duff et al. 2017).
struct Frame {
  float3 t, b, n;
  float3 local(float3 v) const { return float3(dot(v, t), dot(v, b), dot(v, n)); }
  float3 world(float3 v) const { return t * v.x + b * v.y + n * v.z; }
};

Frame frameAround(float3 n) {
  float s = n.z >= 0.0f ? 1.0f : -1.0f;
  float a = -1.0f / (s + n.z);
  float k = n.x * n.y * a;
  return {float3(1.0f + s * n.x * n.x * a, s * k, -s * n.x), float3(k, s + n.y * n.y * a, -n.y), n};
}

float3 cosineSample(float2 u) {
  float r = sqrt(u.x), phi = 2.0f * kPi * u.y;
  return float3(r * cos(phi), r * sin(phi), sqrt(max(0.0f, 1.0f - u.x)));
}

// Uniform point on the unit disk, or on a regular polygon for shaped bokeh.
float2 apertureSample(float2 u, uint blades) {
  if (blades < 3) return sqrt(u.x) * float2(cos(2.0f * kPi * u.y), sin(2.0f * kPi * u.y));
  float wedge = floor(u.x * blades), r = sqrt(u.x * blades - wedge);
  float a0 = 0.3f + 2.0f * kPi * wedge / blades, a1 = a0 + 2.0f * kPi / blades;
  return r * ((1.0f - u.y) * float2(cos(a0), sin(a0)) + u.y * float2(cos(a1), sin(a1)));
}

// --- Intersection -------------------------------------------------------------

struct Hit {
  float  t;
  uint   triangle;
  float2 uv;
};

float slab(float3 o, float3 invDir, float3 lo, float3 hi, float tMax) {
  float3 t0 = (lo - o) * invDir, t1 = (hi - o) * invDir;
  float3 tn = min(t0, t1), tf = max(t0, t1);
  float  enter = max(max(tn.x, tn.y), max(tn.z, 0.0f));
  float  exit  = min(min(tf.x, tf.y), min(tf.z, tMax));
  return enter <= exit ? enter : kMiss;
}

// Closest hit, or any hit below tMax when `shadow` is set.
bool trace(float3 o, float3 d, float tMax, bool shadow, device const BvhNode* nodes, device const Triangle* tris,
    thread Hit& hit) {
  float3 invDir = 1.0f / select(d, copysign(float3(1e-20f), d), abs(d) < 1e-20f);
  hit.t         = tMax;
  bool found    = false;
  if (slab(o, invDir, nodes[0].min, nodes[0].max, tMax) == kMiss) return false;

  uint stack[64], top = 0, node = 0;
  while (true) {
    device const BvhNode& n = nodes[node];
    if (n.count > 0) {
      for (uint i = n.first; i < n.first + n.count; ++i) {
        // Möller–Trumbore
        float3 e1 = tris[i].e1.xyz, e2 = tris[i].e2.xyz;
        float3 p   = cross(d, e2);
        float  det = dot(e1, p);
        if (abs(det) < 1e-14f) continue;
        float  inv = 1.0f / det;
        float3 s   = o - tris[i].v0.xyz;
        float  u   = dot(s, p) * inv;
        float3 q   = cross(s, e1);
        float  v   = dot(d, q) * inv;
        float  t   = dot(e2, q) * inv;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f || t <= 1e-6f || t >= hit.t) continue;
        hit   = {t, i, float2(u, v)};
        found = true;
        if (shadow) return true;
      }
      if (top == 0) break;
      node = stack[--top];
      continue;
    }
    // Visit the nearer child first.
    uint  left = n.first, right = left + 1;
    float dl   = slab(o, invDir, nodes[left].min, nodes[left].max, hit.t);
    float dr   = slab(o, invDir, nodes[right].min, nodes[right].max, hit.t);
    uint  near = dr < dl ? right : left, far = dr < dl ? left : right;
    if (min(dl, dr) == kMiss) {
      if (top == 0) break;
      node = stack[--top];
    } else {
      node = near;
      if (max(dl, dr) != kMiss && top < 64) stack[top++] = far;
    }
  }
  return found;
}

// --- GGX microfacets (isotropic, height-correlated Smith) ---------------------

float ggxD(float cosH, float a2) {
  float d = cosH * cosH * (a2 - 1.0f) + 1.0f;
  return a2 / (kPi * d * d);
}

float ggxG1(float c, float a2) {
  c = abs(c);
  return 2.0f * c / (c + sqrt(a2 + (1.0f - a2) * c * c));
}

float ggxG2(float co, float ci, float a2) {
  co = abs(co), ci = abs(ci);
  return 2.0f * co * ci / max(ci * sqrt(a2 + (1.0f - a2) * co * co) + co * sqrt(a2 + (1.0f - a2) * ci * ci), 1e-20f);
}

// Visible-normal sampling with spherical caps (Dupuy & Benyoub 2023).
float3 sampleVisibleNormal(float3 wo, float alpha, float2 u) {
  float3 w   = normalize(float3(wo.xy * alpha, wo.z));
  float  phi = 2.0f * kPi * u.x;
  float  z   = (1.0f - u.y) * (1.0f + w.z) - w.z;
  float  s   = sqrt(saturate(1.0f - z * z));
  float3 h   = float3(s * cos(phi), s * sin(phi), z) + w;
  return normalize(float3(h.xy * alpha, max(h.z, 1e-6f)));
}

float3 schlick(float3 f0, float c) { return f0 + (1.0f - f0) * pow(saturate(1.0f - c), 5.0f); }

// Unpolarised Fresnel for a dielectric; eta = n_inside / n_outside.
float fresnel(float cosI, float eta) {
  float sin2T = (1.0f - cosI * cosI) / (eta * eta);
  if (sin2T >= 1.0f) return 1.0f;
  float cosT = sqrt(1.0f - sin2T);
  float rs = (cosI - eta * cosT) / (cosI + eta * cosT), rp = (eta * cosI - cosT) / (eta * cosI + cosT);
  return 0.5f * (rs * rs + rp * rp);
}

// Restores the energy single-scattering GGX loses on rough metals (Turquin 2019),
// using the white-metal albedo table computed on the CPU.
float3 multiScatter(float3 f0, float cosO, float roughness, device const float* table) {
  float               x = saturate(cosO) * (kAlbedoTableSize - 1), y = saturate(roughness) * (kAlbedoTableSize - 1);
  uint                x0 = min(uint(x), kAlbedoTableSize - 2), y0 = min(uint(y), kAlbedoTableSize - 2);
  device const float* r = table + y0 * kAlbedoTableSize + x0;
  float E = mix(mix(r[0], r[1], x - x0), mix(r[kAlbedoTableSize], r[kAlbedoTableSize + 1], x - x0), y - y0);
  return 1.0f + f0 * (1.0f - E) / max(E, 1e-3f);
}

// --- BSDFs (local frame: normal = +z, wo.z > 0) -------------------------------

struct Sample {
  float3 wi;
  float3 weight;  // f · cos / pdf
  float  pdf;
  bool   delta;  // can't be hit by light sampling, so no MIS
};

bool usesLightSampling(Material m) {
  return m.type == MaterialDiffuse || m.type == MaterialPlastic || (m.type == MaterialMetal && m.roughness >= 0.15f);
}

float plasticSpecularChance(Material m, float fo) {
  return clamp(fo / max(fo + (1.0f - fo) * luminance(m.albedo), 1e-6f), 0.15f, 0.9f);
}

float3 evalBsdf(Material m, float3 wo, float3 wi, device const float* table, thread float& pdf) {
  pdf = 0.0f;
  if (wo.z <= 0.0f || wi.z <= 0.0f) return 0.0f;
  float3 albedo = m.albedo, h = normalize(wo + wi);
  float  a2 = pow(max(m.roughness * m.roughness, kSharp), 2.0f);

  if (m.type == MaterialDiffuse) {
    pdf = wi.z / kPi;
    return albedo / kPi;
  }
  if (m.type == MaterialMetal) {
    float D = ggxD(h.z, a2);
    pdf     = D * ggxG1(wo.z, a2) / (4.0f * wo.z);
    return schlick(albedo, dot(wo, h)) * D * ggxG2(wo.z, wi.z, a2) / (4.0f * wo.z * wi.z) *
           multiScatter(albedo, wo.z, m.roughness, table);
  }
  if (m.type == MaterialPlastic) {
    float  fo = fresnel(wo.z, m.ior), ps = plasticSpecularChance(m, fo);
    float3 diffuse = albedo / kPi * (1.0f - fo) * (1.0f - fresnel(wi.z, m.ior));
    pdf            = (1.0f - ps) * wi.z / kPi;
    if (m.roughness * m.roughness < kSharp) return diffuse;
    float D = ggxD(h.z, a2);
    pdf += ps * D * ggxG1(wo.z, a2) / (4.0f * wo.z);
    return diffuse + fresnel(dot(wo, h), m.ior) * D * ggxG2(wo.z, wi.z, a2) / (4.0f * wo.z * wi.z);
  }
  return 0.0f;
}

bool sampleBsdf(Material m, float3 wo, bool outside, device const float* table, thread uint& rng, thread Sample& s) {
  float alpha   = m.roughness * m.roughness;
  bool  sharp   = alpha < kSharp;
  alpha         = max(alpha, kSharp);
  float  a2     = alpha * alpha;
  float  pick   = rand(rng);
  float2 u      = rand2(rng);
  float3 mirror = float3(-wo.x, -wo.y, wo.z);
  s.pdf         = 0.0f;
  s.delta       = false;

  switch (m.type) {
    case MaterialDiffuse:
      s.wi     = cosineSample(u);
      s.pdf    = s.wi.z / kPi;
      s.weight = m.albedo;
      return true;

    case MaterialMetal: {
      if (sharp) {
        s = {mirror, schlick(m.albedo, wo.z), 0.0f, true};
        return true;
      }
      float3 h = sampleVisibleNormal(wo, alpha, u);
      s.wi     = reflect(-wo, h);
      if (s.wi.z <= 0.0f) return false;
      float G1 = ggxG1(wo.z, a2);
      s.pdf    = ggxD(h.z, a2) * G1 / (4.0f * wo.z);
      s.weight = schlick(m.albedo, dot(wo, h)) * ggxG2(wo.z, s.wi.z, a2) / G1 *
                 multiScatter(m.albedo, wo.z, m.roughness, table);
      s.delta = !usesLightSampling(m);
      return true;
    }

    case MaterialPlastic: {
      float fo = fresnel(wo.z, m.ior), ps = plasticSpecularChance(m, fo);
      if (sharp && pick < ps) {
        s = {mirror, float3(fo / ps), 0.0f, true};
        return true;
      }
      s.wi = pick < ps ? reflect(-wo, sampleVisibleNormal(wo, alpha, u)) : cosineSample(u);
      if (s.wi.z <= 0.0f) return false;
      s.weight = evalBsdf(m, wo, s.wi, table, s.pdf) * s.wi.z / max(s.pdf, 1e-20f);
      return s.pdf > 0.0f;
    }

    case MaterialGlass: {
      // Walter et al. 2007. Choosing reflection vs refraction by Fresnel cancels it from the weight.
      float  eta = outside ? m.ior : 1.0f / m.ior;
      float3 h   = sharp ? float3(0, 0, 1) : sampleVisibleNormal(wo, alpha, u);
      float  c   = dot(wo, h);
      if (c <= 0.0f) return false;
      bool reflects = pick < fresnel(c, eta);
      if (reflects) {
        s.wi = reflect(-wo, h);
      } else {
        float cosT = sqrt(max(0.0f, 1.0f - (1.0f - c * c) / (eta * eta)));
        s.wi       = normalize(-wo / eta + (c / eta - cosT) * h);
      }
      if (reflects != (s.wi.z > 0.0f)) return false;
      float g  = sharp ? 1.0f : ggxG2(wo.z, s.wi.z, a2) / ggxG1(wo.z, a2);
      s.weight = reflects ? float3(g) : m.albedo * g;
      s.delta  = true;
      return true;
    }
  }
  return false;
}

// --- Lights -------------------------------------------------------------------

struct LightSample {
  float3 dir, radiance;
  float  dist, pdf;
};

bool sampleLight(float3 p, constant Uniforms& U, device const LightRef* lights, device const Triangle* tris,
    device const TriangleShading* shading, device const Material* materials, thread uint& rng, thread LightSample& ls) {
  // Binary search the power CDF.
  float u  = rand(rng);
  uint  lo = 0, hi = U.lightCount - 1;
  while (lo < hi) {
    uint mid = (lo + hi) / 2;
    if (lights[mid].cdf < u)
      lo = mid + 1;
    else
      hi = mid;
  }
  uint                   id  = lights[lo].triangle;
  device const Triangle& tri = tris[id];

  float2 r   = rand2(rng);
  float3 q   = tri.v0.xyz + sqrt(r.x) * ((1.0f - r.y) * tri.e1.xyz + r.y * tri.e2.xyz);
  float3 d   = q - p;
  ls.dist    = length(d);
  ls.dir     = d / ls.dist;
  float cosL = -dot(ls.dir, normalize(cross(tri.e1.xyz, tri.e2.xyz)));
  if (cosL <= 1e-6f) return false;
  ls.radiance = materials[shading[id].material].emission;
  // Picking ∝ power makes the area pdf luminance(Le) / total power for every light.
  ls.pdf = luminance(ls.radiance) / U.lightPower * ls.dist * ls.dist / cosL;
  return true;
}

float3 sky(float3 d, constant Uniforms& U) {
  return d.y >= 0.0f ? mix(U.skyHorizon, U.skyZenith, sqrt(d.y))
                     : mix(U.skyHorizon, U.skyGround, saturate(sqrt(-d.y) * 1.5f));
}

// --- Kernel -------------------------------------------------------------------

kernel void pathTrace(device float4* accumulation [[buffer(0)]], constant Uniforms& U [[buffer(1)]],
    device const BvhNode* nodes [[buffer(2)]], device const Triangle* tris [[buffer(3)]],
    device const TriangleShading* shading [[buffer(4)]], device const Material* materials [[buffer(5)]],
    device const LightRef* lights [[buffer(6)]], device const float* albedoTable [[buffer(7)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= U.resolution.x || gid.y >= U.resolution.y) return;
  uint pixel = gid.y * U.resolution.x + gid.x;
  uint rng   = pcg(pixel ^ pcg(U.sampleIndex * 0x9E3779B9u + 0x7F4A7C15u));

  // Camera ray through a jittered pixel position, then through the lens.
  float2 film = (float2(gid) + rand2(rng)) / float2(U.resolution) * 2.0f - 1.0f;
  float3 dir  = normalize(U.cameraForward + film.x * U.cameraRight - film.y * U.cameraUp);
  float3 org  = U.cameraPosition;
  if (U.aperture > 0.0f) {
    float3 focus = org + dir * (U.focusDistance / dot(dir, U.cameraForward));
    float2 lens  = apertureSample(rand2(rng), U.blades) * U.aperture;
    org += lens.x * normalize(U.cameraRight) + lens.y * normalize(U.cameraUp);
    dir = normalize(focus - org);
  }

  float3 radiance = 0.0f, throughput = 1.0f;
  float  lastPdf   = 0.0f;
  bool   lastDelta = true;

  for (uint bounce = 0;; ++bounce) {
    float limit = bounce == 0 ? 0.0f : U.clamp;  // only indirect light is clamped
    Hit   hit;
    if (!trace(org, dir, kMiss, false, nodes, tris, hit)) {
      radiance += clampTo(throughput * sky(dir, U), limit);
      break;
    }

    device const Triangle&        tri = tris[hit.triangle];
    device const TriangleShading& sh  = shading[hit.triangle];
    Material                      m   = materials[sh.material];
    float3                        p   = tri.v0.xyz + tri.e1.xyz * hit.uv.x + tri.e2.xyz * hit.uv.y;
    float3                        ng  = normalize(cross(tri.e1.xyz, tri.e2.xyz));
    float3                        ns  = normalize(
        float3(sh.n0) * (1.0f - hit.uv.x - hit.uv.y) + float3(sh.n1) * hit.uv.x + float3(sh.n2) * hit.uv.y);
    bool outside = dot(dir, ng) < 0.0f;

    if (!outside && m.type == MaterialGlass) throughput *= exp(-float3(m.absorption) * hit.t);  // Beer–Lambert

    if (m.type == MaterialLight) {
      if (bounce == 0 && U.hideLights) {  // studio trick: the camera sees through lights
        org = nudge(p, dir);
        bounce--;
        continue;
      }
      if (outside) {
        float w = 1.0f;
        if (!lastDelta) w = misWeight(lastPdf, luminance(m.emission) / U.lightPower * hit.t * hit.t / -dot(dir, ng));
        radiance += clampTo(throughput * float3(m.emission) * w, limit);
      }
      break;
    }
    if (bounce >= U.maxBounces) break;

    if (!outside) ng = -ng, ns = -ns;
    float3 wo = -dir;
    if (dot(ns, wo) <= 1e-4f) ns = ng;  // interpolated normal faces away: use the real one
    Frame  f   = frameAround(ns);
    float3 woL = f.local(wo);

    // Next-event estimation, weighted against BSDF sampling.
    LightSample ls;
    if (U.lightCount > 0 && usesLightSampling(m) && sampleLight(p, U, lights, tris, shading, materials, rng, ls) &&
        dot(ls.dir, ng) > 0.0f) {
      float  bsdfPdf;
      float3 wiL = f.local(ls.dir);
      float3 fr  = evalBsdf(m, woL, wiL, albedoTable, bsdfPdf);
      Hit    blocker;
      if (maxOf(fr) > 0.0f && !trace(nudge(p, ng), ls.dir, ls.dist * 0.999f, true, nodes, tris, blocker))
        radiance += clampTo(throughput * fr * wiL.z * ls.radiance * misWeight(ls.pdf, bsdfPdf) / ls.pdf, limit);
    }

    Sample s;
    if (woL.z <= 0.0f || !sampleBsdf(m, woL, outside, albedoTable, rng, s)) break;
    float3 wi       = f.world(s.wi);
    bool   transmit = s.wi.z < 0.0f;
    if (transmit != (dot(wi, ng) < 0.0f)) break;  // crossed the real surface: shading-normal leak

    throughput *= s.weight;
    lastPdf   = s.pdf;
    lastDelta = s.delta;
    org       = nudge(p, transmit ? -ng : ng);
    dir       = wi;

    if (bounce >= 3) {  // Russian roulette
      float survive = clamp(maxOf(throughput), 0.05f, 0.95f);
      if (rand(rng) > survive) break;
      throughput /= survive;
    }
  }

  if (!all(isfinite(radiance))) radiance = 0.0f;
  accumulation[pixel] = (U.sampleIndex == 0 ? 0.0f : accumulation[pixel]) + float4(radiance, 1.0f);
}

// Built-in scenes. Units are metres, +y is up, the default view looks down -z.

#include <cstdio>

#include "scene.hpp"

using namespace shapes;

const float3 kGold{1.0f, 0.71f, 0.29f}, kCopper{0.95f, 0.64f, 0.54f}, kChrome = splat(0.95f);

static void studio(Scene& s, const std::string& objPath) {
  s.add(cyclorama(), plastic(float3{0.13f, 0.125f, 0.12f}, 0.4f));

  std::string error;
  if (auto model = objPath.empty() ? std::nullopt : loadObj(objPath, error)) {
    // Scale to 2.4 units and stand it on the floor.
    float3 lo = splat(1e30f), hi = splat(-1e30f);
    for (float3 p : model->positions) lo = simd::min(lo, p), hi = simd::max(hi, p);
    float3 base{(lo.x + hi.x) / 2, lo.y, (lo.z + hi.z) / 2};
    for (float3& p : model->positions) p = (p - base) * (2.4f / simd::reduce_max(hi - lo));
    s.add(*model, glass(0, float3{0.6f, 0.85f, 0.95f}));
  } else {
    if (!error.empty()) std::fprintf(stderr, "warning: %s\n", error.c_str());
    s.add(trefoil(float3{0, 1.45f, 0}, 0.36f, 0.17f), glass(0, float3{0.55f, 0.82f, 0.95f}));
  }

  s.add(sphere(float3{-2.35f, 0.65f, 0.7f}, 0.65f), metal(kGold, 0.18f));
  s.add(sphere(float3{2.35f, 0.65f, 0.6f}, 0.65f), glass(0.35f, float3{0.85f, 0.92f, 0.97f}));
  s.add(sphere(float3{-1.05f, 0.32f, 2.1f}, 0.32f), plastic(float3{0.85f, 0.1f, 0.06f}, 0.12f));
  s.add(sphere(float3{0.95f, 0.3f, 2.25f}, 0.3f), metal(kChrome, 0));
  s.add(torus(float3{0, 0.07f, 2.6f}, 0.32f, 0.07f), metal(kCopper, 0.25f));

  s.add(panel(float3{0, 5.5f, 1.6f}, float3{0, -1, 0.3f}, 4.5f, 2.5f), light(float3{1, 0.95f, 0.88f}, 7));  // key
  s.add(
      panel(float3{-6.5f, 2.6f, -0.8f}, float3{1, 0, 0.45f}, 0.9f, 5), light(float3{1, 0.72f, 0.45f}, 9));  // warm rim
  s.add(
      panel(float3{6.5f, 2.6f, -0.8f}, float3{-1, 0, 0.45f}, 0.9f, 5), light(float3{0.5f, 0.75f, 1}, 11));  // cool rim
  s.add(panel(float3{0, 3, 13}, float3{0, -0.2f, -1}, 8, 4), light(splat(1), 0.5f));                        // fill

  s.skyZenith = float3{0.012f, 0.014f, 0.018f}, s.skyHorizon = splat(0.008f), s.skyGround = splat(0.004f);
  s.exposure   = -0.3f;
  s.hideLights = true;
  s.camera     = {.target = float3{0, 1.05f, 0.6f},
          .pitch          = 0.13f,
          .distance       = 9.5f,
          .fov            = 30,
          .aperture       = 0.05f,
          .focusDistance  = 9.2f};
}

static void neon(Scene& s) {
  s.add(cyclorama(), plastic(splat(0.015f), 0.1f));  // black, wet-looking floor

  const float3 pink{1, 0.08f, 0.45f}, cyan{0.05f, 0.7f, 1}, amber{1, 0.45f, 0.08f};
  s.add(torus(float3{-0.75f, 2.05f, -1.9f}, 1.65f, 0.045f, rotateX(M_PI / 2)), light(pink, 7));
  s.add(torus(float3{1.0f, 1.75f, -1.3f}, 1.35f, 0.045f, rotateY(1.1f) * rotateX(M_PI / 2)), light(cyan, 7));
  for (float x : {-3.9f, -3.4f, 3.4f, 3.9f})  // standing bars
    s.add(box(float3{x - 0.03f, 0.2f, -1.23f}, float3{x + 0.03f, 3.4f, -1.17f}), light(x < 0 ? cyan : pink, 5));
  s.add(torus(float3{0, 1.8f, 11}, 4, 0.08f, rotateX(M_PI / 2)),
      light(amber, 6));  // behind the camera, seen in reflections

  s.add(sphere(float3{0, 0.8f, 0.4f}, 0.8f), metal(kChrome, 0));
  s.add(sphere(float3{-1.95f, 0.5f, 1.1f}, 0.5f), glass(0));
  s.add(trefoil(float3{1.9f, 0.75f, 1.0f}, 0.22f, 0.1f), glass(0, float3{0.85f, 0.95f, 1}));

  s.exposure = 0.3f;
  s.camera   = {.target = float3{0, 1.0f, 0.3f},
        .pitch          = 0.06f,
        .distance       = 8.5f,
        .fov            = 32,
        .aperture       = 0.04f,
        .focusDistance  = 8.1f};
}

static void materials(Scene& s) {
  s.add(cyclorama(), diffuse(splat(0.3f)));
  const float roughness[] = {0, 0.1f, 0.2f, 0.35f, 0.55f, 0.8f};
  for (int i = 0; i < 6; ++i) {
    float x = i - 2.5f, r = roughness[i];
    s.add(sphere(float3{x, 0.4f, -1.3f}, 0.4f), glass(r, float3{0.7f, 0.9f, 0.95f}));
    s.add(sphere(float3{x, 0.4f, 0}, 0.4f), plastic(float3{0.02f, 0.32f, 0.36f}, r));
    s.add(sphere(float3{x, 0.4f, 1.3f}, 0.4f), metal(kGold, r));
  }
  s.add(panel(float3{0, 6, 3.5f}, float3{0, -1, -0.6f}, 7, 2.5f), light(float3{1, 0.96f, 0.9f}, 6));
  s.add(panel(float3{0, 1.6f, -3.5f}, float3{0, 0, 1}, 9, 0.25f), light(float3{0.8f, 0.9f, 1}, 10));

  s.skyZenith = splat(0.05f), s.skyHorizon = splat(0.03f), s.skyGround = splat(0.01f);
  s.hideLights = true;
  s.camera     = {.target = float3{0, 0.2f, 0.15f}, .pitch = 0.6f, .distance = 7.6f, .fov = 31};
}

static void cornell(Scene& s) {
  Material white = diffuse(float3{0.725f, 0.71f, 0.68f});
  s.add(quad(float3{0, 0, 0}, float3{0, 0, 1}, float3{1, 0, 0}), white);   // floor
  s.add(quad(float3{0, 2, 0}, float3{1, 0, 0}, float3{0, 0, 1}), white);   // ceiling
  s.add(quad(float3{0, 1, -1}, float3{1, 0, 0}, float3{0, 1, 0}), white);  // back
  s.add(quad(float3{-1, 1, 0}, float3{0, 1, 0}, float3{0, 0, 1}), diffuse(float3{0.63f, 0.065f, 0.05f}));
  s.add(quad(float3{1, 1, 0}, float3{0, 0, 1}, float3{0, 1, 0}), diffuse(float3{0.14f, 0.45f, 0.091f}));
  s.add(box(float3{-0.3f, 0, -0.3f}, float3{0.3f, 1.2f, 0.3f})
            .transform(translate(float3{-0.33f, 0, -0.28f}) * rotateY(0.3f)),
      metal(splat(0.92f), 0.3f));
  s.add(sphere(float3{0.4f, 0.38f, 0.35f}, 0.38f), glass(0));
  s.add(quad(float3{0, 1.999f, 0}, float3{0.235f, 0, 0}, float3{0, 0, 0.19f}), light(float3{17, 12, 4}, 1));
  s.camera = {.target = float3{0, 1, 0}, .pitch = 0, .distance = 3.9f, .fov = 39.3f, .focusDistance = 3.9f};
}

const char* const kSceneNames[4] = {"studio", "neon", "materials", "cornell"};

std::unique_ptr<Scene> makeScene(const std::string& name, const std::string& objPath) {
  auto s = std::make_unique<Scene>();
  if (name == "studio")
    studio(*s, objPath);
  else if (name == "neon")
    neon(*s);
  else if (name == "materials")
    materials(*s);
  else if (name == "cornell")
    cornell(*s);
  else
    return nullptr;
  s->name = name;
  s->build();
  return s;
}

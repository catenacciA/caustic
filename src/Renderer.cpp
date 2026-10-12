// metal-cpp is header-only; this file provides its implementation.
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "renderer.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <stdexcept>

namespace {

// The shader sources, baked into the binary by the Makefile.
const char* kShaderSource =
#include "shaders.inc"
    ;

// Same visible-normal sampling as the shader.
float3 visibleNormal(float3 wo, float alpha, float u1, float u2) {
  float3 w   = simd::normalize(float3{wo.x * alpha, wo.y * alpha, wo.z});
  float  phi = 2 * M_PI * u1, z = (1 - u2) * (1 + w.z) - w.z, s = std::sqrt(std::max(0.f, 1 - z * z));
  float3 h = float3{s * std::cos(phi), s * std::sin(phi), z} + w;
  return simd::normalize(float3{h.x * alpha, h.y * alpha, std::max(h.z, 1e-6f)});
}

// Average reflectance of a white GGX metal per (roughness, cosθ). The shader uses
// it to add back the energy that single-scattering microfacet models lose.
std::vector<float> ggxAlbedoTable() {
  const int          n = kAlbedoTableSize, strata = 48;
  std::vector<float> table(n * n);
  for (int row = 0; row < n; ++row)
    for (int col = 0; col < n; ++col) {
      float  roughness = float(row) / (n - 1), alpha = std::max(roughness * roughness, 1e-3f), a2 = alpha * alpha;
      float  c = std::max(float(col) / (n - 1), 1e-3f);
      float3 wo{std::sqrt(1 - c * c), 0, c};
      auto   lambda = [&](float z) { return std::sqrt(a2 + (1 - a2) * z * z); };
      double sum    = 0;
      for (int i = 0; i < strata; ++i)
        for (int j = 0; j < strata; ++j) {
          float3 h  = visibleNormal(wo, alpha, (i + 0.5f) / strata, (j + 0.5f) / strata);
          float3 wi = 2 * simd::dot(wo, h) * h - wo;
          if (wi.z > 0) sum += wi.z * (c + lambda(c)) / (wi.z * lambda(c) + c * lambda(wi.z));  // G2 / G1
        }
      table[row * n + col] = float(sum / (strata * strata));
    }
  return table;
}

template <typename T>
NS::SharedPtr<MTL::Buffer> upload(MTL::Device* device, const std::vector<T>& data) {
  return NS::TransferPtr(
      device->newBuffer(data.data(), std::max<size_t>(data.size(), 1) * sizeof(T), MTL::ResourceStorageModeShared));
}

}  // namespace

Renderer::Renderer(MTL::Device* device)
    : _device(NS::RetainPtr(device)), _queue(NS::TransferPtr(device->newCommandQueue())) {
  NS::Error* error   = nullptr;
  auto       library = NS::TransferPtr(
      device->newLibrary(NS::String::string(kShaderSource, NS::UTF8StringEncoding), nullptr, &error));
  if (!library) throw std::runtime_error(error->localizedDescription()->utf8String());
  auto function = [&](const char* name) {
    return NS::TransferPtr(library->newFunction(NS::String::string(name, NS::UTF8StringEncoding)));
  };

  _tracePipeline   = NS::TransferPtr(device->newComputePipelineState(function("pathTrace").get(), &error));
  _resolvePipeline = NS::TransferPtr(device->newComputePipelineState(function("resolve").get(), &error));

  auto desc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
  desc->setVertexFunction(function("blitVertex").get());
  desc->setFragmentFunction(function("blitFragment").get());
  desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
  _presentPipeline = NS::TransferPtr(device->newRenderPipelineState(desc.get(), &error));
  if (!_tracePipeline || !_resolvePipeline || !_presentPipeline) throw std::runtime_error("failed to create pipelines");

  _albedoTable = upload(device, ggxAlbedoTable());
}

void Renderer::load(const Scene& scene) {
  _scene     = &scene;
  _nodes     = upload(_device.get(), scene.nodes);
  _triangles = upload(_device.get(), scene.triangles);
  _shading   = upload(_device.get(), scene.shading);
  _materials = upload(_device.get(), scene.materials);
  _lights    = upload(_device.get(), scene.lights);
  reset();
}

void Renderer::resize(uint32_t width, uint32_t height) {
  width = std::max(width, 1u), height = std::max(height, 1u);
  if (width == _width && height == _height) return;
  _width = width, _height = height;
  _accumulation = NS::TransferPtr(_device->newBuffer(size_t(width) * height * 16, MTL::ResourceStorageModePrivate));
  auto desc     = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, width, height, false);
  desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
  desc->setStorageMode(MTL::StorageModeShared);
  _image = NS::TransferPtr(_device->newTexture(desc));
  reset();
}

Uniforms Renderer::uniforms(const Camera& cam) const {
  float3       forward = cam.forward(), right = simd::normalize(simd::cross(forward, float3{0, 1, 0}));
  float        h = std::tan(cam.fov * float(M_PI) / 360);
  const Scene& s = *_scene;
  return {
      .cameraPosition = cam.position(),
      .cameraRight    = right * h * float(_width) / float(_height),
      .cameraUp       = simd::cross(right, forward) * h,
      .cameraForward  = forward,
      .skyZenith      = s.skyZenith,
      .skyHorizon     = s.skyHorizon,
      .skyGround      = s.skyGround,
      .resolution     = {_width, _height},
      .sampleIndex    = _samples,
      .maxBounces     = maxBounces,
      .aperture       = cam.aperture,
      .focusDistance  = cam.focusDistance,
      .blades         = cam.blades,
      .lightCount     = uint(s.lights.size()),
      .lightPower     = s.lightPower,
      .clamp          = 24,
      .exposure       = exposure + s.exposure,
      .hideLights     = uint(s.hideLights),
  };
}

void Renderer::trace(MTL::CommandBuffer* cmd, const Camera& camera, uint32_t samples) {
  if (samples == 0) return;
  auto enc = cmd->computeCommandEncoder();
  enc->setComputePipelineState(_tracePipeline.get());
  MTL::Buffer* buffers[]  = {_accumulation.get(), nullptr, _nodes.get(), _triangles.get(), _shading.get(),
       _materials.get(), _lights.get(), _albedoTable.get()};
  NS::UInteger offsets[8] = {};
  enc->setBuffers(buffers, offsets, NS::Range(0, 8));
  Uniforms u = uniforms(camera);
  for (uint32_t i = 0; i < samples; ++i, ++_samples) {
    u.sampleIndex = _samples;
    enc->setBytes(&u, sizeof u, 1);
    enc->dispatchThreads(MTL::Size(_width, _height, 1), MTL::Size(8, 8, 1));
  }
  enc->endEncoding();
}

void Renderer::resolve(MTL::CommandBuffer* cmd) {
  Uniforms u{.resolution = {_width, _height}, .exposure = exposure + _scene->exposure};
  auto     enc = cmd->computeCommandEncoder();
  enc->setComputePipelineState(_resolvePipeline.get());
  enc->setBuffer(_accumulation.get(), 0, 0);
  enc->setBytes(&u, sizeof u, 1);
  enc->setTexture(_image.get(), 0);
  enc->dispatchThreads(MTL::Size(_width, _height, 1), MTL::Size(16, 16, 1));
  enc->endEncoding();
}

void Renderer::present(MTL::CommandBuffer* cmd, MTL::Texture* target) {
  auto pass = MTL::RenderPassDescriptor::renderPassDescriptor();
  pass->colorAttachments()->object(0)->setTexture(target);
  pass->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionDontCare);
  auto enc = cmd->renderCommandEncoder(pass);
  enc->setRenderPipelineState(_presentPipeline.get());
  enc->setFragmentTexture(_image.get(), 0);
  enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
  enc->endEncoding();
}

bool Renderer::savePng(const std::string& path) {
  auto cmd = _queue->commandBuffer();
  resolve(cmd);
  cmd->commit();
  cmd->waitUntilCompleted();

  std::vector<uint8_t> pixels(size_t(_width) * _height * 4);
  _image->getBytes(pixels.data(), _width * 4, MTL::Region(0, 0, _width, _height), 0);

  auto url      = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), path.size(), false);
  auto space    = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  auto provider = CGDataProviderCreateWithData(nullptr, pixels.data(), pixels.size(), nullptr);
  auto image    = CGImageCreate(_width, _height, 8, 32, _width * 4, space, CGBitmapInfo(kCGImageAlphaNoneSkipLast),
         provider, nullptr, false, kCGRenderingIntentDefault);
  auto dest     = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
  bool ok       = dest && image;
  if (ok) {
    CGImageDestinationAddImage(dest, image, nullptr);
    ok = CGImageDestinationFinalize(dest);
  }
  for (CFTypeRef ref : {(CFTypeRef)dest, (CFTypeRef)image, (CFTypeRef)provider, (CFTypeRef)space, (CFTypeRef)url})
    if (ref) CFRelease(ref);
  return ok;
}

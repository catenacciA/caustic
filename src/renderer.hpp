#pragma once

#include <Metal/Metal.hpp>

#include "scene.hpp"

// GPU side: pipelines, scene buffers, the accumulation buffer and the output image.
class Renderer {
 public:
  explicit Renderer(MTL::Device* device);

  void load(const Scene& scene);
  void resize(uint32_t width, uint32_t height);
  void reset() { _samples = 0; }

  void trace(MTL::CommandBuffer* cmd, const Camera& camera, uint32_t samples);  // add samples per pixel
  void resolve(MTL::CommandBuffer* cmd);                                        // tonemap into the output image
  void present(MTL::CommandBuffer* cmd, MTL::Texture* target);                  // stretch it over `target`
  bool savePng(const std::string& path);                                        // resolve, wait, write

  MTL::CommandQueue* queue() const { return _queue.get(); }
  uint32_t           width() const { return _width; }
  uint32_t           height() const { return _height; }
  uint32_t           samples() const { return _samples; }

  float    exposure   = 0;  // EV, on top of the scene's own
  uint32_t maxBounces = 12;

 private:
  Uniforms uniforms(const Camera& camera) const;

  NS::SharedPtr<MTL::Device>               _device;
  NS::SharedPtr<MTL::CommandQueue>         _queue;
  NS::SharedPtr<MTL::ComputePipelineState> _tracePipeline, _resolvePipeline;
  NS::SharedPtr<MTL::RenderPipelineState>  _presentPipeline;
  NS::SharedPtr<MTL::Buffer>  _nodes, _triangles, _shading, _materials, _lights, _albedoTable, _accumulation;
  NS::SharedPtr<MTL::Texture> _image;

  const Scene* _scene   = nullptr;
  uint32_t     _width   = 0;
  uint32_t     _height  = 0;
  uint32_t     _samples = 0;
};

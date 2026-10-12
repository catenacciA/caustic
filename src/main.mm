// Command line, offline rendering, and the interactive AppKit viewer.

#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

#include "renderer.hpp"

static const char* kUsage =
    "usage: caustic [--scene studio|neon|materials|cornell] [--obj model.obj] [--aperture R] [--focus D]\n"
    "               [--output image.png [--size 1920x1080] [--spp 1024]]\n";

struct Options {
  std::string scene = "studio", obj, output;
  uint32_t    width = 1280, height = 800, spp = 1024;
  float       aperture = -1, focus = -1;
};

static std::unique_ptr<Scene> loadScene(const Options& o, const std::string& name) {
  auto scene = makeScene(name, o.obj);
  if (scene && o.aperture >= 0) scene->camera.aperture = o.aperture;
  if (scene && o.focus > 0) scene->camera.focusDistance = o.focus;
  return scene;
}

// --- Offline rendering --------------------------------------------------------

static int renderToFile(id<MTLDevice> device, const Options& o) {
  auto scene = loadScene(o, o.scene);
  if (!scene) return std::fprintf(stderr, "unknown scene '%s'\n%s", o.scene.c_str(), kUsage), 1;

  Renderer renderer((__bridge MTL::Device*)device);
  renderer.load(*scene);
  renderer.resize(o.width, o.height);

  auto start = std::chrono::steady_clock::now();
  for (uint32_t done = 0; done < o.spp;) @autoreleasepool {
      uint32_t batch = std::min(4u, o.spp - done);  // short command buffers stay clear of the GPU watchdog
      auto     cmd   = renderer.queue()->commandBuffer();
      renderer.trace(cmd, scene->camera, batch);
      cmd->commit();
      cmd->waitUntilCompleted();
      std::fprintf(stderr, "\r%s: %u/%u spp", scene->name.c_str(), done += batch, o.spp);
    }
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::fprintf(stderr, " in %.1fs (%.1fM paths/s)\n", seconds, 1e-6 * o.width * o.height * o.spp / seconds);

  if (!renderer.savePng(o.output)) return std::fprintf(stderr, "could not write %s\n", o.output.c_str()), 1;
  return 0;
}

// --- Interactive viewer -------------------------------------------------------

@interface CausticView : MTKView <MTKViewDelegate>
@end

@implementation CausticView {
  Options                              _options;
  std::unique_ptr<Renderer>            _renderer;
  std::unique_ptr<Scene>               _scene;
  Camera                               _home;
  bool                                 _moving, _retonemap, _dragged;
  uint32_t                             _samplesPerFrame;
  std::shared_ptr<std::atomic<double>> _msPerSample;  // written by Metal's completion handler

  NSVisualEffectView* _hud;
  NSTextField*        _hudText;
  NSString*           _notice;
  CFTimeInterval      _noticeTime, _hudTime;
}

static const uint32_t kMaxSamples = 16384;

- (instancetype)initWithFrame:(NSRect)frame options:(const Options&)options {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!(self = [super initWithFrame:frame device:device])) return nil;
  self.colorPixelFormat         = MTLPixelFormatBGRA8Unorm;
  self.preferredFramesPerSecond = 120;
  self.delegate                 = self;

  _options         = options;
  _renderer        = std::make_unique<Renderer>((__bridge MTL::Device*)device);
  _msPerSample     = std::make_shared<std::atomic<double>>(0);
  _samplesPerFrame = 1;
  _renderer->resize(frame.size.width, frame.size.height);
  [self showScene:options.scene];

  _hud                          = [[NSVisualEffectView alloc] init];
  _hud.material                 = NSVisualEffectMaterialHUDWindow;
  _hud.blendingMode             = NSVisualEffectBlendingModeWithinWindow;
  _hud.wantsLayer               = YES;
  _hud.layer.cornerRadius       = 10;
  _hudText                      = [NSTextField labelWithString:@""];
  _hudText.maximumNumberOfLines = 3;
  [_hud addSubview:_hudText];
  [self addSubview:_hud];
  return self;
}

- (void)showScene:(const std::string&)name {
  _scene = loadScene(_options, name);
  _home  = _scene->camera;
  _renderer->load(*_scene);
  [self restart];
}

- (void)restart {
  _renderer->reset();
  _moving = true;
}

- (void)notify:(NSString*)text {
  _notice     = text;
  _noticeTime = CACurrentMediaTime();
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
  _renderer->resize(self.bounds.size.width, self.bounds.size.height);
  [self restart];
}

- (void)drawInMTKView:(MTKView*)view {
  bool converged = _renderer->samples() >= kMaxSamples;
  if (!_moving && !_retonemap && converged) return [self updateHud];

  // One sample per frame while the camera moves, then fill a ~14 ms GPU budget.
  uint32_t samples = converged ? 0 : _moving ? 1 : _samplesPerFrame;
  _moving = _retonemap = false;

  id<CAMetalDrawable> drawable = view.currentDrawable;
  if (!drawable) return;
  auto cmd = _renderer->queue()->commandBuffer();
  _renderer->trace(cmd, _scene->camera, samples);
  _renderer->resolve(cmd);
  _renderer->present(cmd, (__bridge MTL::Texture*)drawable.texture);
  cmd->presentDrawable((__bridge MTL::Drawable*)drawable);
  if (samples) {
    auto timing = _msPerSample;
    cmd->addCompletedHandler(^(MTL::CommandBuffer* done) {
      timing->store((done->GPUEndTime() - done->GPUStartTime()) * 1e3 / samples);
    });
  }
  cmd->commit();

  if (double ms = _msPerSample->load(); ms > 0) _samplesPerFrame = uint32_t(std::clamp(14.0 / ms, 1.0, 32.0));
  [self updateHud];
}

- (void)updateHud {
  CFTimeInterval now = CACurrentMediaTime();
  if (_hud.hidden || now - _hudTime < 0.2) return;
  _hudTime = now;

  const Camera& c      = _scene->camera;
  NSString*     status = [NSString stringWithFormat:@"%s  ·  %u spp  ·  lens %.3f  ·  focus %.2f  ·  EV %+.2f%@",
      _scene->name.c_str(), _renderer->samples(), c.aperture, c.focusDistance, _renderer->exposure,
      _notice && now - _noticeTime < 4 ? [@"  ·  " stringByAppendingString:_notice] : @""];
  NSString*     help   = @"drag orbit  ·  ⌥ drag pan  ·  scroll zoom  ·  click to focus\n"
                         @"[ ] aperture  ·  B bokeh  ·  − = exposure  ·  1–4 scene  ·  P save  ·  R reset  ·  H hide";

  NSMutableAttributedString* text = [[NSMutableAttributedString alloc]
      initWithString:[status stringByAppendingString:@"\n"]
          attributes:@{NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium]}];
  [text appendAttributedString:[[NSAttributedString alloc]
                                   initWithString:help
                                       attributes:@{
                                         NSFontAttributeName : [NSFont systemFontOfSize:11],
                                         NSForegroundColorAttributeName : NSColor.secondaryLabelColor
                                       }]];
  _hudText.attributedStringValue = text;
  NSSize size                    = _hudText.fittingSize;
  _hudText.frame                 = NSMakeRect(12, 8, size.width, size.height);
  _hud.frame                     = NSMakeRect(14, 14, size.width + 24, size.height + 16);
}

// --- Input --------------------------------------------------------------------

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)orbit:(NSEvent*)e {
  _scene->camera.yaw -= e.deltaX * 0.006f;
  _scene->camera.pitch = std::clamp(_scene->camera.pitch + float(e.deltaY) * 0.006f, -0.1f, 1.45f);
  [self restart];
}

- (void)pan:(NSEvent*)e {
  Camera& c     = _scene->camera;
  float3  right = simd::normalize(simd::cross(c.forward(), float3{0, 1, 0}));
  c.target += (-float(e.deltaX) * right + float(e.deltaY) * simd::cross(right, c.forward())) * c.distance * 0.0015f;
  [self restart];
}

- (void)zoom:(float)factor {
  Camera& c       = _scene->camera;
  float   old     = c.distance;
  c.distance      = std::clamp(c.distance * factor, 0.5f, 60.f);
  c.focusDistance = std::max(0.05f, c.focusDistance + c.distance - old);  // keep the subject sharp
  [self restart];
}

- (void)mouseDown:(NSEvent*)e {
  _dragged = false;
}
- (void)mouseDragged:(NSEvent*)e {
  _dragged = true;
  e.modifierFlags& NSEventModifierFlagOption ? [self pan:e] : [self orbit:e];
}
- (void)rightMouseDragged:(NSEvent*)e {
  [self pan:e];
}
- (void)scrollWheel:(NSEvent*)e {
  [self zoom:std::exp(-e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 0.004 : 0.08))];
}
- (void)magnifyWithEvent:(NSEvent*)e {
  [self zoom:std::exp(-e.magnification)];
}

// A click without a drag focuses the lens on whatever is under the cursor.
- (void)mouseUp:(NSEvent*)e {
  if (_dragged) return;
  NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  Camera& c = _scene->camera;
  float3  f = c.forward(), right = simd::normalize(simd::cross(f, float3{0, 1, 0})), up = simd::cross(right, f);
  float   h = std::tan(c.fov * float(M_PI) / 360);
  float3  d = simd::normalize(
      f + right * h * float((2 * p.x / self.bounds.size.width - 1) * self.bounds.size.width / self.bounds.size.height) +
      up * h * float(2 * p.y / self.bounds.size.height - 1));
  if (auto t = _scene->intersect(c.position(), d)) {
    c.focusDistance = *t * simd::dot(d, f);
    [self notify:[NSString stringWithFormat:@"focused at %.2f", c.focusDistance]];
    [self restart];
  }
}

- (void)keyDown:(NSEvent*)e {
  Camera&   c   = _scene->camera;
  NSString* s   = e.charactersIgnoringModifiers.lowercaseString;
  unichar   key = s.length ? [s characterAtIndex:0] : 0;
  switch (key) {
    case '1':
    case '2':
    case '3':
    case '4': return [self showScene:kSceneNames[key - '1']];
    case ']': c.aperture = c.aperture <= 0 ? 0.01f : std::min(c.aperture * 1.25f, 1.f); break;
    case '[': c.aperture = c.aperture < 0.0105f ? 0 : c.aperture / 1.25f; break;
    case 'b':
      c.blades = c.blades == 0 ? 5 : c.blades == 5 ? 6 : c.blades == 6 ? 8 : 0;
      [self notify:c.blades ? [NSString stringWithFormat:@"%u-blade aperture", c.blades] : @"round aperture"];
      break;
    case 'r': c = _home, _renderer->exposure = 0; break;
    case '=':
    case '-':  // exposure only affects tonemapping, so samples are kept
      _renderer->exposure += key == '=' ? 0.25f : -0.25f;
      _retonemap = true;
      return;
    case 'h': _hud.hidden = !_hud.hidden; return;
    case 'p': {
      NSString* path = [NSString stringWithFormat:@"%@/caustic-%s-%u.png",
          NSFileManager.defaultManager.currentDirectoryPath, _scene->name.c_str(), _renderer->samples()];
      [self notify:_renderer->savePng(path.UTF8String) ? [@"saved " stringByAppendingString:path.lastPathComponent]
                                                       : @"save failed"];
      return;
    }
    default: return [super keyDown:e];
  }
  [self restart];
}

@end

// --- App ----------------------------------------------------------------------

@interface                   AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic) Options options;
@property NSWindow*          window;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)n {
  NSMenu*     bar  = [[NSMenu alloc] init];
  NSMenuItem* item = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
  item.submenu     = [[NSMenu alloc] init];
  [item.submenu addItemWithTitle:@"Quit Caustic" action:@selector(terminate:) keyEquivalent:@"q"];
  [item.submenu addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
  NSApp.mainMenu = bar;

  NSRect frame                       = NSMakeRect(0, 0, _options.width, _options.height);
  _window                            = [[NSWindow alloc]
      initWithContentRect:frame
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                          NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView
                  backing:NSBackingStoreBuffered
                    defer:NO];
  _window.title                      = @"Caustic";
  _window.titlebarAppearsTransparent = YES;
  _window.appearance                 = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  try {
    _window.contentView = [[CausticView alloc] initWithFrame:frame options:_options];
  } catch (const std::exception& e) {
    std::fprintf(stderr, "caustic: %s\n", e.what());
    exit(1);
  }
  [_window makeFirstResponder:_window.contentView];
  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  return YES;
}

@end

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    Options o;
    try {
      for (int i = 1; i < argc; i += 2) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") return std::fputs(kUsage, stdout), 0;
        if (i + 1 == argc) throw std::invalid_argument(arg);
        std::string value = argv[i + 1];
        if (arg == "--scene")
          o.scene = value;
        else if (arg == "--obj")
          o.obj = value;
        else if (arg == "--output" || arg == "-o")
          o.output = value;
        else if (arg == "--spp")
          o.spp = std::stoul(value);
        else if (arg == "--aperture")
          o.aperture = std::stof(value);
        else if (arg == "--focus")
          o.focus = std::stof(value);
        else if (arg != "--size" || std::sscanf(value.c_str(), "%ux%u", &o.width, &o.height) != 2)
          throw std::invalid_argument(arg);
      }
    } catch (const std::exception&) {
      return std::fputs(kUsage, stderr), 2;
    }
    if (std::find(std::begin(kSceneNames), std::end(kSceneNames), o.scene) == std::end(kSceneNames))
      return std::fprintf(stderr, "unknown scene '%s'\n%s", o.scene.c_str(), kUsage), 2;

    try {
      if (!o.output.empty()) return renderToFile(MTLCreateSystemDefaultDevice(), o);
      static AppDelegate* app                  = [[AppDelegate alloc] init];  // NSApp only holds it weakly
      app.options                              = o;
      NSApplication.sharedApplication.delegate = app;
      [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
      [NSApp run];
    } catch (const std::exception& e) {
      return std::fprintf(stderr, "caustic: %s\n", e.what()), 1;
    }
  }
}

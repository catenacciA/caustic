CXX      := clang++
CXXFLAGS := -std=c++20 -O3 -Wall -Wextra -Wno-unused-parameter -isystem third-party/metal-cpp -Ibuild -MMD -MP
LDFLAGS  := -framework Metal -framework MetalKit -framework QuartzCore -framework Cocoa -framework ImageIO

# Some Command Line Tools installs ship an empty libc++ header folder; use the SDK's copy instead.
ifeq ($(wildcard $(dir $(shell xcrun --find clang++))../include/c++/v1/cstdint),)
  CXXFLAGS += -stdlib++-isystem $(shell xcrun --show-sdk-path)/usr/include/c++/v1
endif

OBJECTS := build/main.o build/renderer.o build/scene.o build/scenes.o
SHADERS := src/shaders/shared.h src/shaders/pathtracer.metal src/shaders/display.metal

build/caustic: $(OBJECTS)
	$(CXX) $^ $(LDFLAGS) -o $@

build/%.o: src/%.cpp | build/shaders.inc
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/%.o: src/%.mm
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@

# Shaders are compiled at runtime (no Xcode needed), so embed their source as a
# string literal. #line keeps compiler errors pointing at the real files.
build/shaders.inc: $(SHADERS)
	@mkdir -p build
	@{ echo 'R"MSL('; for f in $^; do echo "#line 1 \"$$f\""; sed 's/^#include "shared.h"$$//' $$f; done; echo ')MSL"'; } > $@

# The README images (~25 minutes on an M1).
renders: build/caustic
	@mkdir -p docs
	build/caustic --scene studio    --size 1920x1080 --spp 4096 -o docs/studio.png
	build/caustic --scene neon      --size 1920x1080 --spp 4096 -o docs/neon.png
	build/caustic --scene materials --size 1600x900  --spp 2048 -o docs/materials.png
	build/caustic --scene studio    --size 1600x900  --spp 2048 --aperture 0.3 --focus 7.6 -o docs/depth-of-field.png
	build/caustic --scene cornell   --size 1024x1024 --spp 4096 -o docs/cornell.png

clean:
	rm -rf build

.PHONY: renders clean
-include $(OBJECTS:.o=.d)

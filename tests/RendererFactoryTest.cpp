// Guards the M17 public-header surface (docs/ARCHITECTURE.md §12).
//
// Every include below is from include/ ONLY — no src/ header. That is the point
// of the test: it stands in for an out-of-tree game, which sees only the public
// API. If a future change leaks a private (src/) type into one of these public
// headers, this translation unit stops compiling — a compile-time tripwire on the
// committed surface. It also proves createRenderer() links (the bgfx-backed
// concrete renderer is reachable purely through the abstract Renderer handle, with
// bgfx itself never named here).

#include "renderer/RendererFactory.h"
#include "renderer/Renderer.h"
#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "WorldCoord.h"

#include <gtest/gtest.h>

#include <memory>

// The factory hands back a live renderer behind the abstract interface. We drive
// only the device-free part of the contract (camera + per-voxel submission); a
// real device needs a window, which a headless test has not got. Destruction is
// safe without initialize() because Renderer::shutdown() no-ops when uninitialized.
TEST(RendererFactory, ReturnsUsableAbstractRenderer) {
    std::unique_ptr<Renderer> renderer = createRenderer();
    ASSERT_NE(renderer, nullptr);

    // Exercise the device-free part of the contract through the abstract
    // interface alone (camera state + per-voxel submission just mutate CPU-side
    // state). render()/initialize()/setViewport() talk to the GPU device and need
    // a real window, which a headless test has not got.
    renderer->setCameraPosition(WorldCoord(10.0, 20.0, 30.0));
    renderer->setCameraRotation(0.1f, 0.2f, 0.0f);
    renderer->drawVoxel(WorldCoord(0.0, 0.0, 0.0), 0xff00ff00);
}

// The other front-end leaves promoted in M17 are constructible from include/ too:
// a game builds its layer stack and engine without reaching into src/.
TEST(RendererFactory, PublicFrontEndTypesAreSelfContained) {
    LayerConfig config = LayerConfig::loadFromString(R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
)");
    EXPECT_EQ(config.layers().size(), 1u);

    Engine engine;  // lifecycle type, header-only dependencies in include/
    EXPECT_FALSE(engine.getIsRunning());
}

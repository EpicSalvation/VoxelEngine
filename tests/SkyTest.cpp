// Tests for standard skybox support (M17): the SkyParams gradient curve
// (include/renderer/Sky.h) and the procedural-sky reference plugin, which the
// host queries each frame and forwards to Renderer::setSky.
//
// The renderer's GPU side is not unit-testable without a device, but skyColor() is
// the exact CPU mirror of the per-vertex gradient the renderer bakes into the
// background sphere (BgfxRenderer::render), so pinning it here pins the gradient
// the sky shows — including the byte-identical no-sky guarantee (enabled == false).

#include "renderer/Sky.h"
#include "procedural_sky.h"
#include "plugin_api.h"

#include <gtest/gtest.h>

#include <cmath>

// The procedural-sky plugin is compiled into this test binary (see CMakeLists.txt),
// so its api() inline static resolves in one address space. It registers no engine
// hooks, so we call its init directly (no PluginContext needed).
extern "C" int procedural_sky_plugin_init(PluginContext* ctx);

namespace {
void expectColorNear(const glm::vec3& got, const glm::vec3& want, float eps = 1e-4f) {
    EXPECT_NEAR(got.r, want.r, eps);
    EXPECT_NEAR(got.g, want.g, eps);
    EXPECT_NEAR(got.b, want.b, eps);
}
}  // namespace

// ── SkyParams gradient curve ────────────────────────────────────────────────

// The default SkyParams draws no sky: enabled is false so the renderer skips the
// background sphere entirely and the flat clear color shows through — the
// byte-identical guarantee.
TEST(Sky, DefaultIsDisabled) {
    SkyParams s;
    EXPECT_FALSE(s.enabled);
}

// The gradient hits each stop exactly along the up axis: zenith straight up,
// horizon perpendicular to up, ground straight down.
TEST(Sky, GradientHitsStopsAlongUpAxis) {
    SkyParams s;  // default day-ish stops
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    expectColorNear(skyColor(s, glm::vec3(0, 1, 0), up), s.zenith);
    expectColorNear(skyColor(s, glm::vec3(1, 0, 0), up), s.horizon);
    expectColorNear(skyColor(s, glm::vec3(0, 0, 1), up), s.horizon);
    expectColorNear(skyColor(s, glm::vec3(0, -1, 0), up), s.ground);
}

// The gradient is measured against the supplied up vector, not a fixed +Y — so on
// an arbitrary surface (camera up = a surface normal) the horizon stays level: with
// up = +X, looking +X is the zenith and looking +Y is the horizon.
TEST(Sky, GradientTracksCameraUp) {
    SkyParams s;
    const glm::vec3 upX(1.0f, 0.0f, 0.0f);
    expectColorNear(skyColor(s, glm::vec3(1, 0, 0), upX), s.zenith);
    expectColorNear(skyColor(s, glm::vec3(0, 1, 0), upX), s.horizon);
    expectColorNear(skyColor(s, glm::vec3(-1, 0, 0), upX), s.ground);
}

// Inputs need not be normalized, and a degenerate (zero-length) up or dir is safe:
// it falls back to the horizon color rather than dividing by zero / NaNing.
TEST(Sky, HandlesUnnormalizedAndDegenerateInputs) {
    SkyParams s;
    // A long +Y ray is still the zenith.
    expectColorNear(skyColor(s, glm::vec3(0, 100, 0), glm::vec3(0, 5, 0)), s.zenith);
    // Zero up / zero dir → horizon, no NaNs.
    const glm::vec3 z0 = skyColor(s, glm::vec3(0, 1, 0), glm::vec3(0, 0, 0));
    const glm::vec3 z1 = skyColor(s, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    expectColorNear(z0, s.horizon);
    expectColorNear(z1, s.horizon);
    EXPECT_FALSE(std::isnan(z0.r));
    EXPECT_FALSE(std::isnan(z1.r));
}

// Between horizon and zenith the gradient is monotone in the up-component: a ray
// tilted further up is strictly closer to the zenith color than a shallower one.
TEST(Sky, GradientMonotoneBetweenStops) {
    SkyParams s;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 low  = skyColor(s, glm::normalize(glm::vec3(1.0f, 0.3f, 0.0f)), up);
    const glm::vec3 high = skyColor(s, glm::normalize(glm::vec3(1.0f, 3.0f, 0.0f)), up);
    // zenith.b (0.90) > horizon.b (0.95)? horizon blue is higher, so a higher ray
    // moves the blue channel toward zenith (lower). Assert the blue decreases and
    // the red moves toward the zenith's lower red.
    EXPECT_LT(high.b, low.b);
    EXPECT_LT(high.r, low.r);
}

// ── procedural-sky plugin ───────────────────────────────────────────────────

TEST(ProceduralSky, FillsApiAndShipsSaneDefaults) {
    ASSERT_EQ(procedural_sky_plugin_init(nullptr), 0);
    ASSERT_NE(psky::api().sample, nullptr);
    ASSERT_NE(psky::api().configure, nullptr);

    const SkyParams s = psky::api().sample(0.0);
    EXPECT_TRUE(s.enabled);                 // a sky is drawn by default (the day sky)
    EXPECT_GT(s.horizon_falloff, 0.0f);
    // The default is the atmospheric day sky: a bright horizon over a bluer zenith.
    expectColorNear(s.horizon, psky::daySky().horizon);
    expectColorNear(s.zenith,  psky::daySky().zenith);
}

// The static default does not animate: sampling at different times returns the same
// look (daynight_hz defaults to 0).
TEST(ProceduralSky, StaticByDefault) {
    ASSERT_EQ(procedural_sky_plugin_init(nullptr), 0);
    const SkyParams a = psky::api().sample(0.0);
    const SkyParams b = psky::api().sample(123.0);
    expectColorNear(a.zenith, b.zenith);
    expectColorNear(a.horizon, b.horizon);
}

// The space preset is a near-uniform dark backdrop — no bright horizon band, which
// is the whole point of the M19 space look.
TEST(ProceduralSky, SpacePresetIsUniformlyDark) {
    const SkyParams s = psky::spaceSky();
    EXPECT_TRUE(s.enabled);
    const float sumZ = s.zenith.r + s.zenith.g + s.zenith.b;
    const float sumH = s.horizon.r + s.horizon.g + s.horizon.b;
    EXPECT_LT(sumZ, 0.2f);
    EXPECT_LT(sumH, 0.2f);
    // No bright horizon band: horizon is within a hair of the zenith brightness.
    EXPECT_LT(std::fabs(sumH - sumZ), 0.1f);
}

// A configured day/night cycle animates the look over time while staying a valid,
// enabled sky throughout; the band eases (raised cosine), so it moves but never
// leaves [0,1] on any channel.
TEST(ProceduralSky, DayNightCycleAnimates) {
    ASSERT_EQ(procedural_sky_plugin_init(nullptr), 0);
    psky::Config cfg;
    cfg.day   = psky::daySky();
    cfg.night = psky::duskSky();
    cfg.daynight_hz = 0.1f;   // ~10 s period
    psky::api().configure(&cfg);

    const SkyParams start = psky::api().sample(0.0);
    bool moved = false;
    for (double t = 0.0; t <= 10.0; t += 0.25) {
        const SkyParams s = psky::api().sample(t);
        EXPECT_TRUE(s.enabled);
        for (int c = 0; c < 3; ++c) {
            EXPECT_GE(s.zenith[c], 0.0f);
            EXPECT_LE(s.zenith[c], 1.0f);
            EXPECT_GE(s.horizon[c], 0.0f);
            EXPECT_LE(s.horizon[c], 1.0f);
        }
        if (std::fabs(s.horizon.r - start.horizon.r) > 1e-3f) moved = true;
    }
    EXPECT_TRUE(moved) << "the day/night cycle should animate the look over time";

    // At phase 0 it is exactly the day look; at the half-period it is peak night.
    expectColorNear(psky::api().sample(0.0).horizon, psky::daySky().horizon);
    expectColorNear(psky::api().sample(5.0).horizon, psky::duskSky().horizon, 2e-3f);
}

// configure() overrides the base look, and sample() reflects it (with animation
// frozen for an exact check).
TEST(ProceduralSky, ConfigureOverridesBase) {
    ASSERT_EQ(procedural_sky_plugin_init(nullptr), 0);
    psky::Config cfg;
    cfg.day.enabled = true;
    cfg.day.zenith  = glm::vec3(0.1f, 0.2f, 0.3f);
    cfg.day.horizon = glm::vec3(0.4f, 0.5f, 0.6f);
    cfg.day.ground  = glm::vec3(0.7f, 0.8f, 0.9f);
    cfg.daynight_hz = 0.0f;   // static
    psky::api().configure(&cfg);

    const SkyParams s = psky::api().sample(999.0);
    expectColorNear(s.zenith,  glm::vec3(0.1f, 0.2f, 0.3f));
    expectColorNear(s.horizon, glm::vec3(0.4f, 0.5f, 0.6f));
    expectColorNear(s.ground,  glm::vec3(0.7f, 0.8f, 0.9f));
}

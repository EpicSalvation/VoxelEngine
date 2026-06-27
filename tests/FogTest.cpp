// Tests for distance-obscurance fog (M17): the FogParams curve (include/renderer/
// Fog.h) and the two reference supplier plugins (atmospheric-mist, range-
// attenuation), which the host queries each frame and forwards to Renderer::setFog.
//
// The renderer's GPU side is not unit-testable without a device, but fogFactor()
// is the exact CPU mirror of the fragment shader's math (shaders/fs_voxel.sc), so
// pinning it here pins the curve the shader applies — including the byte-identical
// no-fog guarantee (density 0 → factor 0 at every distance).

#include "renderer/Fog.h"
#include "atmospheric_mist.h"
#include "range_attenuation.h"
#include "plugin_api.h"

#include <gtest/gtest.h>

#include <cmath>

// The fog plugins are compiled into this test binary (see CMakeLists.txt), so
// their api() inline statics resolve in one address space. They register no engine
// hooks, so we call their init directly (no PluginContext needed).
extern "C" int atmospheric_mist_plugin_init(PluginContext* ctx);
extern "C" int range_attenuation_plugin_init(PluginContext* ctx);

// ── FogParams curve ─────────────────────────────────────────────────────────

// The default FogParams disables fog: the factor is 0 at every distance, so the
// shader's mix() returns the un-fogged fragment — the byte-identical guarantee.
TEST(Fog, DefaultIsDisabled) {
    FogParams f;  // all-zero
    for (float d : {0.0f, 1.0f, 100.0f, 1e6f})
        EXPECT_EQ(fogFactor(f, d), 0.0f);
}

// A density-0 band of any near/far is still fully disabled (factor 0 everywhere).
TEST(Fog, ZeroDensityIsAlwaysOff) {
    FogParams f;
    f.near_m = 10.0f; f.far_m = 200.0f; f.density = 0.0f;
    EXPECT_EQ(fogFactor(f, 0.0f),   0.0f);
    EXPECT_EQ(fogFactor(f, 150.0f), 0.0f);
    EXPECT_EQ(fogFactor(f, 1000.0f),0.0f);
}

// The near→far ramp: clamped to 0 at/below near, to density at/beyond far, and
// linear in between.
TEST(Fog, LinearRampClampedToBand) {
    FogParams f;
    f.near_m = 100.0f; f.far_m = 300.0f; f.density = 0.8f;

    EXPECT_FLOAT_EQ(fogFactor(f, 0.0f),   0.0f);   // before the band
    EXPECT_FLOAT_EQ(fogFactor(f, 100.0f), 0.0f);   // at near
    EXPECT_FLOAT_EQ(fogFactor(f, 200.0f), 0.4f);   // midpoint → density/2
    EXPECT_FLOAT_EQ(fogFactor(f, 300.0f), 0.8f);   // at far → full density
    EXPECT_FLOAT_EQ(fogFactor(f, 500.0f), 0.8f);   // beyond far → clamped
}

// A degenerate band (far <= near) does not divide by zero and still saturates to
// density past the near plane (the max(_,1e-4) guard the shader also uses).
TEST(Fog, DegenerateBandIsSafe) {
    FogParams f;
    f.near_m = 50.0f; f.far_m = 50.0f; f.density = 1.0f;
    EXPECT_FLOAT_EQ(fogFactor(f, 40.0f), 0.0f);
    EXPECT_FLOAT_EQ(fogFactor(f, 60.0f), 1.0f);
    EXPECT_FALSE(std::isnan(fogFactor(f, 50.0f)));
}

// ── atmospheric-mist plugin ─────────────────────────────────────────────────

TEST(AtmosphericMist, FillsApiAndShipsSaneDefaults) {
    ASSERT_EQ(atmospheric_mist_plugin_init(nullptr), 0);
    ASSERT_NE(mist::api().sample, nullptr);
    ASSERT_NE(mist::api().configure, nullptr);

    const FogParams f = mist::api().sample(0.0);
    EXPECT_GT(f.far_m, f.near_m);          // a real band
    EXPECT_GT(f.density, 0.0f);            // enabled by default (this is the haze)
    EXPECT_LE(f.density, 1.0f);
}

// The haze breathes: density drifts over time but stays within [0,1], and the
// band (near/far) is held fixed — only the strength animates.
TEST(AtmosphericMist, DensityBreathesWithinBounds) {
    ASSERT_EQ(atmospheric_mist_plugin_init(nullptr), 0);
    const FogParams a = mist::api().sample(0.0);
    bool moved = false;
    for (double t = 0.0; t <= 40.0; t += 0.5) {
        const FogParams f = mist::api().sample(t);
        EXPECT_GE(f.density, 0.0f);
        EXPECT_LE(f.density, 1.0f);
        EXPECT_EQ(f.near_m, a.near_m);     // band fixed
        EXPECT_EQ(f.far_m,  a.far_m);
        if (std::fabs(f.density - a.density) > 1e-4f) moved = true;
    }
    EXPECT_TRUE(moved) << "density should animate over time";
}

// configure() overrides the base look, and sample() reflects it.
TEST(AtmosphericMist, ConfigureOverridesBase) {
    ASSERT_EQ(atmospheric_mist_plugin_init(nullptr), 0);
    mist::Config cfg;
    cfg.base.color   = glm::vec3(1.0f, 0.0f, 0.0f);
    cfg.base.near_m  = 5.0f;
    cfg.base.far_m   = 9.0f;
    cfg.base.density = 0.5f;
    cfg.drift_amplitude = 0.0f;  // freeze the breathing for an exact check
    mist::api().configure(&cfg);

    const FogParams f = mist::api().sample(123.0);
    EXPECT_FLOAT_EQ(f.near_m, 5.0f);
    EXPECT_FLOAT_EQ(f.far_m,  9.0f);
    EXPECT_FLOAT_EQ(f.density, 0.5f);
    EXPECT_FLOAT_EQ(f.color.r, 1.0f);
}

// ── range-attenuation plugin ────────────────────────────────────────────────

TEST(RangeAttenuation, FillsApiAndModelsACave) {
    ASSERT_EQ(range_attenuation_plugin_init(nullptr), 0);
    ASSERT_NE(rangefog::api().sample, nullptr);

    const FogParams f = rangefog::api().sample(0.0);
    EXPECT_GT(f.far_m, f.near_m);
    EXPECT_FLOAT_EQ(f.density, 1.0f);      // beyond the lit radius the cave is dark
    // The target color is near-black (darkness), not the bright sky a haze uses.
    EXPECT_LT(f.color.r + f.color.g + f.color.b, 0.2f);
}

// The torch flickers: the lit radius wavers over time but stays ordered
// (near < far) and non-negative — no inverted or negative band ever escapes.
TEST(RangeAttenuation, FlickerKeepsBandValid) {
    ASSERT_EQ(range_attenuation_plugin_init(nullptr), 0);
    bool moved = false;
    const FogParams a = rangefog::api().sample(0.0);
    for (double t = 0.0; t <= 5.0; t += 0.01) {
        const FogParams f = rangefog::api().sample(t);
        EXPECT_GE(f.near_m, 0.0f);
        EXPECT_GT(f.far_m, f.near_m);
        if (std::fabs(f.far_m - a.far_m) > 1e-3f) moved = true;
    }
    EXPECT_TRUE(moved) << "the lit radius should flicker over time";
}

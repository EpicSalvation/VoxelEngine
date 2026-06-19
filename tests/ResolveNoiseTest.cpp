// ResolveNoiseTest — M16 C2: plugins consume the engine noise registry through
// ctx->resolve_noise.
//
// Covers the README acceptance items:
//   - a plugin resolves a built-in noise id by name (returns the registry fn);
//   - a plugin resolves a register_noise-overridden id (the plugin fn wins);
//   - an unknown id resolves to nullptr (the §6 contract — fail loudly);
//   - the migrated base-terrain / layered-world generators produce output that
//     matches the resolved built-in "value" noise, i.e. they consume the
//     registry instead of a hand-rolled inline copy (and stay deterministic).

#include "core/PluginManager.h"
#include "world/Noise.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ── Probe plugins exercising ctx->resolve_noise ──────────────────────────────

NoiseFn g_probeValue    = nullptr;
NoiseFn g_probeUnknown  = reinterpret_cast<NoiseFn>(0x1);  // sentinel != nullptr
NoiseFn g_probeOverride = nullptr;

int initResolveProbe(PluginContext* ctx) {
    g_probeValue   = ctx->resolve_noise(ctx, "value");
    g_probeUnknown = ctx->resolve_noise(ctx, "no_such_noise_id");
    return 0;
}

float constHalfNoise(WorldCoord, uint64_t, const RecipeParam*, size_t, void*) {
    return 0.5f;
}

int initResolveOverride(PluginContext* ctx) {
    // Register a same-id override, then resolve it back: the plugin fn must win
    // over the built-in floor.
    ctx->register_noise(ctx, "value", &constHalfNoise, nullptr);
    g_probeOverride = ctx->resolve_noise(ctx, "value");
    return 0;
}

// The built-in "value" noise fn pointer, for equivalence recomputation below.
NoiseFn builtinValue() {
    for (const auto& b : noise::builtins())
        if (std::string(b.id) == "value") return b.fn;
    return nullptr;
}

// Mirror the base-terrain plugin's constants (white-box equivalence check).
namespace base_terrain {
constexpr uint64_t kSeed       = 0x5DEECE66Dull;
constexpr double   kLattice    = 48.0;
constexpr int      kBaseHeight = 6;
constexpr int      kAmplitude  = 22;

int height(int64_t worldX, int64_t worldZ) {
    NoiseFn vn = builtinValue();
    RecipeParam p[1] = {{ "scale", RecipeParamKind::Number, kLattice, nullptr }};
    float n = vn(WorldCoord(static_cast<double>(worldX), 0.0,
                            static_cast<double>(worldZ)), kSeed, p, 1, nullptr);
    return kBaseHeight + static_cast<int>(n * kAmplitude);
}
}  // namespace base_terrain

const RegisteredLayerGenerator* findGen(const PluginManager& pm,
                                        const std::string& name) {
    for (const auto& g : pm.layerGenerators())
        if (g.layer_name == name) return &g;
    return nullptr;
}

}  // namespace

// ── resolve_noise mechanics ──────────────────────────────────────────────────

TEST(ResolveNoise, BuiltinIdResolvesToRegistryFn) {
    PluginManager pm;  // built-in floor exists from construction
    g_probeValue = nullptr;
    ASSERT_NE(pm.wireInPlugin(initResolveProbe), kInvalidPluginId);

    ASSERT_NE(g_probeValue, nullptr);
    // The plugin got exactly the registry's winning fn, which is the built-in.
    const RegisteredNoise* reg = pm.resolveNoise("value");
    ASSERT_NE(reg, nullptr);
    EXPECT_EQ(g_probeValue, reg->fn);
    EXPECT_EQ(g_probeValue, builtinValue());
}

TEST(ResolveNoise, UnknownIdResolvesNull) {
    PluginManager pm;
    g_probeUnknown = reinterpret_cast<NoiseFn>(0x1);
    ASSERT_NE(pm.wireInPlugin(initResolveProbe), kInvalidPluginId);
    EXPECT_EQ(g_probeUnknown, nullptr);
}

TEST(ResolveNoise, OverriddenIdResolvesToPluginFn) {
    PluginManager pm;
    g_probeOverride = nullptr;
    ASSERT_NE(pm.wireInPlugin(initResolveOverride), kInvalidPluginId);
    // The override is the plugin's own fn, not the built-in floor.
    EXPECT_EQ(g_probeOverride, &constHalfNoise);
    EXPECT_NE(g_probeOverride, builtinValue());
}

// ── Migrated generators consume the resolved registry ────────────────────────

#if defined(VOXEL_BASE_PLUGIN_PATH)
TEST(ResolveNoise, BaseTerrainGeneratorMatchesResolvedValueNoise) {
    PluginManager pm;
    ASSERT_NE(pm.loadPlugin(VOXEL_BASE_PLUGIN_PATH), kInvalidPluginId)
        << "could not load base-terrain from " << VOXEL_BASE_PLUGIN_PATH;

    const RegisteredLayerGenerator* gen = findGen(pm, "terrain");
    ASSERT_NE(gen, nullptr);
    ASSERT_NE(gen->fn, nullptr);

    const int grid = 16;
    std::vector<Voxel> out(static_cast<size_t>(grid * grid * grid));
    gen->fn(WorldCoord(0.0, 0.0, 0.0), grid, out.data(), gen->user_data);

    // Independently rebuild the expected heightmap from the built-in value noise:
    // the generator must agree voxel-for-voxel, proving it samples the registry
    // rather than a private inline noise.
    for (int z = 0; z < grid; ++z)
        for (int x = 0; x < grid; ++x) {
            const int h = base_terrain::height(x, z);
            for (int y = 0; y < grid; ++y) {
                const Voxel& v = out[x + grid * (y + grid * z)];
                if (y < 0 || y > h) {
                    EXPECT_TRUE(v.isEmpty()) << "x=" << x << " y=" << y << " z=" << z;
                } else if (y == h) {
                    EXPECT_FALSE(v.isEmpty());
                    EXPECT_EQ(v.material.palette_index, 2);  // grass at the surface
                } else {
                    EXPECT_FALSE(v.isEmpty());
                    EXPECT_EQ(v.material.palette_index, 1);  // stone below
                }
            }
        }
}

TEST(ResolveNoise, BaseTerrainGeneratorIsDeterministic) {
    PluginManager pm;
    ASSERT_NE(pm.loadPlugin(VOXEL_BASE_PLUGIN_PATH), kInvalidPluginId);
    const RegisteredLayerGenerator* gen = findGen(pm, "terrain");
    ASSERT_NE(gen, nullptr);

    const int grid = 12;
    std::vector<Voxel> a(static_cast<size_t>(grid * grid * grid));
    std::vector<Voxel> b(static_cast<size_t>(grid * grid * grid));
    gen->fn(WorldCoord(64.0, 0.0, -32.0), grid, a.data(), gen->user_data);
    gen->fn(WorldCoord(64.0, 0.0, -32.0), grid, b.data(), gen->user_data);
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(Voxel)));
}
#endif  // VOXEL_BASE_PLUGIN_PATH

#if defined(VOXEL_LAYERED_PLUGIN_PATH)
TEST(ResolveNoise, LayeredWorldGeneratorMatchesResolvedValueNoise) {
    PluginManager pm;
    ASSERT_NE(pm.loadPlugin(VOXEL_LAYERED_PLUGIN_PATH), kInvalidPluginId)
        << "could not load layered-world from " << VOXEL_LAYERED_PLUGIN_PATH;

    const RegisteredLayerGenerator* gen = findGen(pm, "terrain");
    ASSERT_NE(gen, nullptr);
    ASSERT_NE(gen->fn, nullptr);

    // layered-world's terrain generator runs at the implicit 1 m scale, sampling
    // the height field at (x + 0.5) m column centers (see terrain_generator).
    constexpr int    kBaseHeight = 6;
    constexpr int    kAmplitude  = 22;
    constexpr double kLattice    = 48.0;
    constexpr uint64_t kSeed     = 0x5DEECE66Dull;
    NoiseFn vn = builtinValue();
    auto heightM = [&](double wx, double wz) {
        RecipeParam p[1] = {{ "scale", RecipeParamKind::Number, kLattice, nullptr }};
        float n = vn(WorldCoord(wx, 0.0, wz), kSeed, p, 1, nullptr);
        return static_cast<double>(kBaseHeight) + n * kAmplitude;
    };

    const int grid = 16;
    std::vector<Voxel> out(static_cast<size_t>(grid * grid * grid));
    gen->fn(WorldCoord(0.0, 0.0, 0.0), grid, out.data(), gen->user_data);

    for (int z = 0; z < grid; ++z)
        for (int x = 0; x < grid; ++x) {
            const double h = heightM(x + 0.5, z + 0.5);
            for (int y = 0; y < grid; ++y) {
                const double worldY = static_cast<double>(y);  // cell bottom, 1 m
                const Voxel& v = out[x + grid * (y + grid * z)];
                if (worldY < 0.0 || worldY >= h) {
                    EXPECT_TRUE(v.isEmpty()) << "x=" << x << " y=" << y << " z=" << z;
                } else {
                    EXPECT_FALSE(v.isEmpty()) << "x=" << x << " y=" << y << " z=" << z;
                }
            }
        }
}
#endif  // VOXEL_LAYERED_PLUGIN_PATH

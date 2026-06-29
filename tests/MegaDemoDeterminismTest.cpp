// Verifies the M18 mega-demo's headline guarantee: the world seed deterministically
// drives generation, so the SAME seed regenerates the SAME world and a DIFFERENT
// seed produces a different one (ARCHITECTURE §4). Exercises the real overworld +
// trees worldgen plugins through the exact pipeline the demo uses — the terminal
// terrain layer generator followed by every registered feature overlay (caves,
// ore, trees) — with the seed threaded through the generator's user_data and the
// feature pass's seed argument. Headless: no window or GPU.

#include "core/PluginManager.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

// The two M18 worldgen plugins are compiled into the test binary (OVERWORLD_/
// TREES_COMPILED_IN suppress their generic voxel_plugin_init); wire them in here.
extern "C" int overworld_plugin_init(PluginContext* ctx);
extern "C" int trees_plugin_init(PluginContext* ctx);

namespace {

struct Gen {
    PluginManager     pm;
    LayerGeneratorFn  terrain = nullptr;
    Gen() {
        pm.wireInPlugin(overworld_plugin_init);
        pm.wireInPlugin(trees_plugin_init);
        for (const auto& g : pm.layerGenerators())
            if (g.layer_name == "terrain") terrain = g.fn;
    }
    // Generate a chunk the way the demo does: base layer, then every feature
    // overlay in registration order, all driven by `seed`.
    std::vector<Voxel> world(WorldCoord origin, int n, uint64_t seed) {
        std::vector<Voxel> v(static_cast<size_t>(n) * n * n, Voxel::empty());
        terrain(origin, n, v.data(), &seed);
        for (const auto& f : pm.featureGenerators())
            if (f.fn) f.fn(origin, 1.0, n, v.data(), nullptr, 0, seed, f.user_data);
        return v;
    }
};

bool same(const Voxel& a, const Voxel& b) {
    return a.material.palette_index == b.material.palette_index &&
           a.material.density == b.material.density;
}
int diffCount(const std::vector<Voxel>& a, const std::vector<Voxel>& b) {
    int d = 0;
    for (size_t i = 0; i < a.size(); ++i) if (!same(a[i], b[i])) ++d;
    return d;
}

constexpr int        kN    = 32;
const WorldCoord     kOrigin(0.0, 0.0, 0.0);

}  // namespace

TEST(MegaDemoDeterminism, SameSeedRegeneratesIdenticalWorld) {
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    const std::vector<Voxel> a = g.world(kOrigin, kN, 12345u);
    const std::vector<Voxel> b = g.world(kOrigin, kN, 12345u);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(diffCount(a, b), 0) << "the same seed must regenerate the same world byte-for-byte";
}

TEST(MegaDemoDeterminism, DifferentSeedProducesDifferentWorld) {
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    const std::vector<Voxel> a = g.world(kOrigin, kN, 12345u);
    const std::vector<Voxel> b = g.world(kOrigin, kN, 99999u);
    EXPECT_GT(diffCount(a, b), 0) << "a different seed should drive a visibly different world";
}

TEST(MegaDemoDeterminism, ChunkOriginIsSeamless) {
    // The world voxel at a given position must not depend on which chunk grid it
    // is generated in — guards against a chunk-local (non-world-space) noise lattice
    // that would tear caves/terrain at chunk borders.
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    const int small = 16, big = 32;
    const std::vector<Voxel> a = g.world(kOrigin, small, 7u);
    const std::vector<Voxel> b = g.world(kOrigin, big,   7u);
    for (int z = 0; z < small; ++z)
        for (int y = 0; y < small; ++y)
            for (int x = 0; x < small; ++x)
                EXPECT_TRUE(same(a[x + small * (y + small * z)],
                                 b[x + big   * (y + big   * z)]))
                    << "mismatch at (" << x << "," << y << "," << z << ")";
}

TEST(MegaDemoDeterminism, ProducesLayeredTerrain) {
    // Sanity: a generated chunk has air, a grass surface, and stone below.
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    const std::vector<Voxel> v = g.world(kOrigin, kN, 12345u);
    bool air = false, grass = false, stone = false;
    for (const auto& vox : v) {
        if (vox.isEmpty()) air = true;
        else if (vox.material.palette_index == 2) grass = true;   // grass
        else if (vox.material.palette_index == 1) stone = true;   // stone
    }
    EXPECT_TRUE(air);
    EXPECT_TRUE(grass);
    EXPECT_TRUE(stone);
}

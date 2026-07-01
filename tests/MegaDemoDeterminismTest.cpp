// Verifies the M18.5 mega-demo's headline guarantee: the world seed
// deterministically drives generation, so the SAME seed regenerates the SAME
// world and a DIFFERENT seed produces a different one (ARCHITECTURE §4).
//
// The demo uses a single terminal "terrain" layer (M13 structural collapse and
// its composite "blocks"/"bedrock" scaffolding were removed — see
// demos/20-mega-demo/main.cpp). This test exercises the real overworld + trees
// worldgen plugins through the terminal terrain generator + feature overlays
// (the same path the demo uses for pre-streaming the spawn neighbourhood).

#include "core/PluginManager.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

extern "C" int overworld_plugin_init(PluginContext* ctx);
extern "C" int trees_plugin_init(PluginContext* ctx);
extern "C" void overworld_set_seed_ptr(uint64_t* ptr);

namespace {

struct Gen {
    PluginManager     pm;
    LayerGeneratorFn  terrain = nullptr;
    uint64_t          seed = 0;
    Gen() {
        pm.wireInPlugin(overworld_plugin_init);
        pm.wireInPlugin(trees_plugin_init);
        for (const auto& g : pm.layerGenerators())
            if (g.layer_name == "terrain") terrain = g.fn;
    }
    std::vector<Voxel> world(WorldCoord origin, int n, uint64_t s) {
        seed = s;
        overworld_set_seed_ptr(&seed);
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
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    const std::vector<Voxel> v = g.world(kOrigin, kN, 12345u);
    bool air = false, grass = false, stone = false;
    for (const auto& vox : v) {
        if (vox.isEmpty()) air = true;
        else if (vox.material.palette_index == 2) grass = true;
        else if (vox.material.palette_index == 1) stone = true;
    }
    EXPECT_TRUE(air);
    EXPECT_TRUE(grass);
    EXPECT_TRUE(stone);
}

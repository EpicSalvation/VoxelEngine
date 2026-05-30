// Tests that the example terrain generator is deterministic and chunk-layout
// independent — the property that lets streamed chunks regenerate identically
// and adjacent chunks share a seamless surface. Headless: no window or GPU.

#include "core/PluginManager.h"
#include "plugins/ExamplePlugin.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>
#include <vector>

namespace {

// Fetch the registered "terrain" generator from a freshly wired example plugin.
LayerGeneratorFn terrainGenerator(PluginManager& pm) {
    pm.wireInPlugin(voxel_plugin_init);
    for (const auto& g : pm.layerGenerators())
        if (g.layer_name == "terrain")
            return g.fn;
    return nullptr;
}

std::vector<Voxel> generate(LayerGeneratorFn fn, WorldCoord origin, int n) {
    std::vector<Voxel> out(static_cast<size_t>(n) * n * n, Voxel::empty());
    fn(origin, n, out.data(), nullptr);
    return out;
}

bool sameMaterial(const Voxel& a, const Voxel& b) {
    return a.material.palette_index == b.material.palette_index &&
           a.material.density == b.material.density;
}

}  // namespace

TEST(GeneratorDeterminism, SameOriginProducesIdenticalVoxels) {
    PluginManager pm;
    LayerGeneratorFn fn = terrainGenerator(pm);
    ASSERT_NE(fn, nullptr);

    const int n = 8;
    WorldCoord origin(64.0, 0.0, -32.0);
    std::vector<Voxel> a = generate(fn, origin, n);
    std::vector<Voxel> b = generate(fn, origin, n);

    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i)
        EXPECT_TRUE(sameMaterial(a[i], b[i])) << "voxel " << i << " differs on regeneration";
}

TEST(GeneratorDeterminism, IndependentOfChunkSize) {
    // The same world (x, y, z) must resolve to the same voxel regardless of the
    // chunk grid it is generated in — guards against a chunk-local noise lattice.
    PluginManager pm;
    LayerGeneratorFn fn = terrainGenerator(pm);
    ASSERT_NE(fn, nullptr);

    const WorldCoord origin(0.0, 0.0, 0.0);
    const int small = 8;
    const int big   = 16;
    std::vector<Voxel> a = generate(fn, origin, small);
    std::vector<Voxel> b = generate(fn, origin, big);

    for (int z = 0; z < small; ++z)
        for (int y = 0; y < small; ++y)
            for (int x = 0; x < small; ++x) {
                const Voxel& va = a[x + small * (y + small * z)];
                const Voxel& vb = b[x + big   * (y + big   * z)];
                EXPECT_TRUE(sameMaterial(va, vb))
                    << "mismatch at world (" << x << "," << y << "," << z << ")";
            }
}

TEST(GeneratorDeterminism, ProducesNonTrivialTerrain) {
    // Sanity: a generated chunk should contain both solid and empty voxels
    // (a heightmap, not all-solid or all-empty).
    PluginManager pm;
    LayerGeneratorFn fn = terrainGenerator(pm);
    ASSERT_NE(fn, nullptr);

    const int n = 32;
    std::vector<Voxel> v = generate(fn, WorldCoord(0.0, 0.0, 0.0), n);
    bool anySolid = false, anyEmpty = false;
    for (const auto& vox : v) {
        if (vox.isEmpty()) anyEmpty = true; else anySolid = true;
    }
    EXPECT_TRUE(anySolid);
    EXPECT_TRUE(anyEmpty);
}

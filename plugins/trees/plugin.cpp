// trees plugin — procedural tree placement for the M18 mega-demo.
//
// Registers ONE feature generator, "tree", and nothing else. It is a separate
// plugin from overworld on purpose: the host applies every registered feature
// generator in turn (the demo-03 pattern), so trees compose onto overworld's
// terrain across a plugin boundary with no shared code — drop the plugin and the
// world is treeless, exactly the M4 visible-on-removal contract. Load order puts
// "tree" after overworld's cave/ore and the water flood, so trunks root on the
// final grass surface and never inside a carved cave.
//
// Placement is a pure function of world column and the world seed (the same seed
// the host threads everywhere): a per-column hash decides whether a tree spawns,
// so the SAME seed grows the SAME forest. Trunks/canopy that would cross the
// chunk's grid bound are clipped — acceptable per-chunk behaviour, matching how
// every feature generator stamps within the grid it is handed.

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cstdint>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// Palette indices — MUST match the overworld plugin's materials.
constexpr uint8_t kGrassIdx  = 2;
constexpr uint8_t kLogIdx    = 8;
constexpr uint8_t kLeavesIdx = 4;

MaterialProperties logMaterial() {
    MaterialProperties m;
    m.density = 600.0f; m.structural_strength = 0.60f; m.hardness = 0.35f;
    m.palette_index = kLogIdx;
    return m;
}
MaterialProperties leafMaterial() {
    MaterialProperties m;
    m.density = 200.0f; m.structural_strength = 0.05f; m.hardness = 0.10f;
    m.porosity = 0.40f; m.palette_index = kLeavesIdx;
    return m;
}

// ── Feature generator: trees ─────────────────────────────────────────────────
void tree_feature(WorldCoord origin, double /*voxel_size_m*/, int n, Voxel* inout,
                  const RecipeParam* params, size_t count, uint64_t seed, void*) {
    const double density = recipe_param_num(params, count, "tree_density", 0.018);
    const uint64_t treeSeed = voxel_seed_mix(seed, 0x77EE5u);
    const MaterialProperties log  = logMaterial();
    const MaterialProperties leaf = leafMaterial();

    auto at = [&](int x, int y, int z) -> Voxel& {
        return inout[x + n * (y + n * z)];
    };

    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            // Find the topmost grass voxel in this column (air directly above it).
            int gy = -1;
            for (int y = n - 2; y >= 1; --y) {
                if (at(x, y, z).material.palette_index == kGrassIdx &&
                    at(x, y + 1, z).isEmpty()) { gy = y; break; }
            }
            if (gy < 0) continue;

            // Deterministic per-column spawn decision.
            const int64_t wx = static_cast<int64_t>(origin.value.x) + x;
            const int64_t wz = static_cast<int64_t>(origin.value.z) + z;
            uint64_t st = voxel_seed_mix(treeSeed,
                              voxel_seed_mix(static_cast<uint64_t>(wx) * 0x1f1f1f1fu + 1u,
                                             static_cast<uint64_t>(wz) * 0x9e3779b9u + 7u));
            if (voxel_rng_norm(&st) >= density) continue;

            const int trunkH = 4 + static_cast<int>(voxel_rng_norm(&st) * 3.0f);  // 4..6

            // Trunk: logs straight up from just above the grass.
            for (int t = 1; t <= trunkH; ++t) {
                const int y = gy + t;
                if (y >= n) break;
                at(x, y, z) = Voxel{log};
            }

            // Canopy: a small leaf blob around the top, written only into air so
            // it never carves trunk or terrain. Radius shrinks toward the crown.
            const int topY = gy + trunkH;
            for (int dy = -2; dy <= 1; ++dy) {
                const int y = topY + dy;
                if (y <= gy || y >= n) continue;
                const int r = (dy >= 1) ? 1 : 2;  // narrow crown, wider shoulders
                for (int dz = -r; dz <= r; ++dz)
                    for (int dx = -r; dx <= r; ++dx) {
                        if (dx == 0 && dz == 0 && dy < 1) continue;  // leave the trunk core
                        if (dx * dx + dz * dz > r * r + 1) continue;  // round the corners
                        const int cx = x + dx, cz = z + dz;
                        if (cx < 0 || cx >= n || cz < 0 || cz >= n) continue;
                        Voxel& c = at(cx, y, cz);
                        if (c.isEmpty()) c = Voxel{leaf};
                    }
            }
        }
}

}  // namespace

// Named entry point so the determinism test can compile this plugin in alongside
// overworld without a voxel_plugin_init collision.
VOXEL_PLUGIN_EXPORT int trees_plugin_init(PluginContext* ctx) {
    // Materials are registered by overworld; trees only needs the placement rule.
    ctx->register_feature_generator(ctx, "tree", tree_feature, nullptr);
    return 0;
}

#ifndef TREES_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return trees_plugin_init(ctx);
}
#endif

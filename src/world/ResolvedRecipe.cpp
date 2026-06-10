#include "ResolvedRecipe.h"

#include <cmath>

#include "Chunk.h"
#include "Voxel.h"

namespace {

// Salt mixed into the decomposition seed for the material-distribution noise, so
// the distribution field is decorrelated from the per-feature seeds (which are
// salted by their declared index + 1). Any fixed nonzero constant works; it only
// has to differ from the feature salts.
constexpr uint64_t kDistributionSalt = 0xD15D15D15D15D15Dull;

// Flatten owning RecipeParamValues into the POD RecipeParam array a NoiseFn /
// FeatureGeneratorFn consumes. The returned array's const char* point into the
// source vector's strings, so the source must outlive the array — it does: the
// ResolvedRecipe (and thus its param vectors) lives for the whole fill call.
std::vector<RecipeParam> flatten(const std::vector<RecipeParamValue>& params) {
    std::vector<RecipeParam> out;
    out.reserve(params.size());
    for (const RecipeParamValue& p : params) {
        RecipeParam rp;
        rp.key    = p.key.c_str();
        rp.kind   = p.kind;
        rp.number = p.number;
        rp.text   = p.text.empty() ? nullptr : p.text.c_str();
        out.push_back(rp);
    }
    return out;
}

// Map a noise value to a material by walking the cumulative weights. Given a
// noise field uniform on [0,1) the long-run proportion of each material matches
// its weight; the mapping is monotone, so contiguous noise bands map to the same
// material (spatial coherence). An empty distribution yields empty space.
Voxel sampleDistribution(const ResolvedDistribution& d,
                         const std::vector<RecipeParam>& flatParams,
                         const WorldCoord& center, uint64_t seed) {
    if (d.materials.empty())
        return Voxel::empty();

    float total = 0.0f;
    for (const ResolvedWeight& w : d.materials)
        total += (w.weight > 0.0f) ? w.weight : 0.0f;
    if (total <= 0.0f)
        return Voxel{d.materials.front().props};  // degenerate: all-zero weights

    float n = d.noise ? d.noise(center, seed, flatParams.data(), flatParams.size(),
                                d.noiseUser)
                      : 0.0f;
    if (n < 0.0f) n = 0.0f;
    if (n >= 1.0f) n = 0.99999994f;  // clamp into [0,1)

    const float threshold = n * total;
    float acc = 0.0f;
    for (const ResolvedWeight& w : d.materials) {
        acc += (w.weight > 0.0f) ? w.weight : 0.0f;
        if (threshold < acc)
            return Voxel{w.props};
    }
    return Voxel{d.materials.back().props};
}

}  // namespace

void fillChildChunk(Chunk& chunk, double voxelSizeM, const ResolvedRecipe& recipe,
                    chunkmath::VoxelCoord macroChildMin, int64_t ratio,
                    uint64_t decompSeed) {
    const int n = chunk.size();
    const ChunkCoord cc = chunk.coord();
    const WorldCoord origin = chunk.origin();
    const uint64_t distSeed = voxel_seed_mix(decompSeed, kDistributionSalt);

    // Flat param arrays per distribution, built once (params are constant over
    // the chunk). Only the present boundaries need flattening.
    const std::vector<RecipeParam> flatInterior = flatten(recipe.interior.noiseParams);
    const std::vector<RecipeParam> flatTop =
        recipe.top.present ? flatten(recipe.top.distribution.noiseParams)
                           : std::vector<RecipeParam>{};
    const std::vector<RecipeParam> flatBottom =
        recipe.bottom.present ? flatten(recipe.bottom.distribution.noiseParams)
                              : std::vector<RecipeParam>{};
    const std::vector<RecipeParam> flatSide =
        recipe.side.present ? flatten(recipe.side.distribution.noiseParams)
                            : std::vector<RecipeParam>{};

    // ── (1)+(2) distribution + boundary overrides ────────────────────────────
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const chunkmath::VoxelCoord gv =
                    chunkmath::chunkLocalToVoxel(cc, x, y, z, n);
                const int64_t mx = gv.x - macroChildMin.x;  // position within the
                const int64_t my = gv.y - macroChildMin.y;  // macro voxel's child
                const int64_t mz = gv.z - macroChildMin.z;  // grid, in [0, ratio)

                // Coarse-supersets-fine invariant (M10, ARCHITECTURE §4): child
                // voxels outside the macro voxel's subvolume are left empty. This
                // arises only when child_chunk_world_size > parent_voxel_size (a
                // config with large chunk sizes relative to voxel sizes). Keeping
                // these cells empty ensures no fine-layer voxel exists without a
                // covering coarse-layer voxel.
                if (mx < 0 || mx >= ratio || my < 0 || my >= ratio || mz < 0 || mz >= ratio) {
                    chunk.at(x, y, z) = Voxel::empty();
                    continue;
                }

                // Overlap order bottom -> side -> top (top wins), per §6.
                const ResolvedDistribution* dist = &recipe.interior;
                const std::vector<RecipeParam>* flat = &flatInterior;
                if (recipe.bottom.present && my < recipe.bottom.depth) {
                    dist = &recipe.bottom.distribution; flat = &flatBottom;
                }
                if (recipe.side.present &&
                    (mx < recipe.side.depth || mx >= ratio - recipe.side.depth ||
                     mz < recipe.side.depth || mz >= ratio - recipe.side.depth)) {
                    dist = &recipe.side.distribution; flat = &flatSide;
                }
                if (recipe.top.present && my >= ratio - recipe.top.depth) {
                    dist = &recipe.top.distribution; flat = &flatTop;
                }

                const WorldCoord center(
                    origin.value.x + (static_cast<double>(x) + 0.5) * voxelSizeM,
                    origin.value.y + (static_cast<double>(y) + 0.5) * voxelSizeM,
                    origin.value.z + (static_cast<double>(z) + 0.5) * voxelSizeM);
                chunk.at(x, y, z) = sampleDistribution(*dist, *flat, center, distSeed);
            }

    // ── (3) feature overlays, in declared order ──────────────────────────────
    for (const ResolvedFeature& f : recipe.features) {
        if (!f.fn) continue;
        const std::vector<RecipeParam> flatParams = flatten(f.params);
        const uint64_t fseed = voxel_seed_mix(decompSeed, f.seedSalt);
        f.fn(origin, voxelSizeM, n, chunk.data(),
             flatParams.data(), flatParams.size(), fseed, f.user);
    }
}

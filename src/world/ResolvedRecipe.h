#pragma once

#include <cstdint>
#include <vector>

#include "plugin_api.h"   // MaterialProperties, NoiseFn, FeatureGeneratorFn, RecipeParam
#include "Recipe.h"       // RecipeParamValue (owning param storage)
#include "ChunkCoordMath.h"

class Chunk;

// A composition recipe with every plugin id already resolved to the concrete
// function/value it names (M9, docs/ARCHITECTURE.md §6).
//
// A `Recipe` (src/world/Recipe.h) keeps material/feature/noise references as
// STRINGS. Turning those strings into MaterialProperties / NoiseFn /
// FeatureGeneratorFn requires PluginManager, which the DecompositionWorker must
// not touch (§13). So resolution happens once, on the MAIN thread, at
// decomposition-job-build time (see RecipeResolve.h), producing this value type.
// The worker then consumes a ResolvedRecipe and never consults any registry.
//
// Determinism: a ResolvedRecipe holds only pure function pointers and plain
// values, so filling a child grid from it is a pure function of
// (ResolvedRecipe, macro VoxelCoord, world seed) — the §4 guarantee carries over
// unchanged from the M6 generator path.

// One material in a distribution, with its properties already looked up.
struct ResolvedWeight {
    MaterialProperties props;
    float              weight = 0.0f;  // relative; normalized across the distribution
};

// A material distribution with its noise function resolved and its effective
// (seed-merged) noise params owned. An empty `materials` list produces empty
// voxels (air) — used by the interior of a recipe that only paints boundaries.
struct ResolvedDistribution {
    std::vector<ResolvedWeight>   materials;
    NoiseFn                       noise     = nullptr;  // null => first material everywhere
    void*                         noiseUser = nullptr;
    std::vector<RecipeParamValue> noiseParams;          // effective params, owning
};

// A resolved per-face boundary override. `present == false` leaves the face to
// the interior distribution; `depth` is how many child-voxel layers inward from
// the face it replaces.
struct ResolvedBoundary {
    ResolvedDistribution distribution;
    int                  depth   = 1;
    bool                 present = false;
};

// A resolved feature overlay: the generator fn plus its effective (seed-merged)
// params and the per-feature seed salt used to decorrelate it from siblings.
struct ResolvedFeature {
    FeatureGeneratorFn            fn       = nullptr;
    void*                         user     = nullptr;
    std::vector<RecipeParamValue> params;            // effective params, owning
    uint64_t                      seedSalt = 0;       // mixed into the per-feature seed
};

struct ResolvedRecipe {
    ResolvedDistribution         interior;
    ResolvedBoundary             top;
    ResolvedBoundary             bottom;
    ResolvedBoundary             side;
    std::vector<ResolvedFeature> features;            // applied in declared order
};

// Fill one child chunk from a resolved recipe — the recipe-driven counterpart of
// the M6 "run the child generator over the subvolume" path. One self-contained
// pure pass:
//   (1) sample the material distribution into every voxel,
//   (2) apply boundary overrides on the macro voxel's exposed faces
//       (overlap order bottom -> side -> top; top wins), then
//   (3) run each feature overlay in declared order over the filled grid.
//
// `macroChildMin` is the global child-layer VoxelCoord of the macro voxel's
// minimum corner, and `ratio` is how many child voxels span the macro voxel
// along each axis (chunkmath::layerRatio), so the worker can tell which voxels
// lie on which face without sampling any neighbor (§6/§13). `decompSeed` is the
// deterministic per-decomposition seed derived from (world_seed, macro coord).
void fillChildChunk(Chunk& chunk, double voxelSizeM, const ResolvedRecipe& recipe,
                    chunkmath::VoxelCoord macroChildMin, int64_t ratio,
                    uint64_t decompSeed);

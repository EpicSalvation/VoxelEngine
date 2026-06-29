#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

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
// the face (MacroFace) or carved surface (Surface) it replaces.
struct ResolvedBoundary {
    ResolvedDistribution distribution;
    int                  depth   = 1;
    bool                 present = false;
    BoundaryMode         mode    = BoundaryMode::MacroFace;  // M18.5
};

// A resolved feature overlay: the generator fn plus its effective (seed-merged)
// params and the per-feature seed salt used to decorrelate it from siblings.
struct ResolvedFeature {
    FeatureGeneratorFn            fn       = nullptr;
    void*                         user     = nullptr;
    std::vector<RecipeParamValue> params;            // effective params, owning
    uint64_t                      seedSalt = 0;       // mixed into the per-feature seed
};

// A resolved occupancy (carve) stage (M18.5). `present == false` (the default)
// means "fully solid" — fillChildChunk skips the carve and behaves as before. A
// null `noise` (an unresolved id, caught earlier by validateRecipes) is likewise
// treated as absent, so the worker fails soft rather than dereferencing null.
struct ResolvedOccupancy {
    NoiseFn                       noise     = nullptr;  // null => no carve (treated absent)
    void*                         noiseUser = nullptr;
    float                         threshold = 0.0f;     // solid iff value >= threshold
    std::vector<RecipeParamValue> params;               // effective params, owning
    bool                          present   = false;
};

struct ResolvedRecipe {
    ResolvedOccupancy            occupancy;            // carve stage, run first (M18.5)
    ResolvedDistribution         interior;
    ResolvedBoundary             top;
    ResolvedBoundary             bottom;
    ResolvedBoundary             side;
    std::vector<ResolvedFeature> features;            // applied in declared order
};

// Salt mixed into the per-decomposition seed for the occupancy (carve) field, so
// the carve is decorrelated from the material distribution and feature fields.
// Exposed here (not file-local to ResolvedRecipe.cpp) so the engine-derived
// coarse-occupancy generator (DecompositionManager, M18.5) samples the *same*
// seeded field a macro's decomposition will, keeping coarse occupancy an exact
// match for fine occupancy.
inline constexpr uint64_t kRecipeOccupancySalt = 0x0CC0CC0CC0CC0CC0ull;

// Fill one child chunk from a resolved recipe — the recipe-driven counterpart of
// the M6 "run the child generator over the subvolume" path. One self-contained
// pure pass:
//   (0) if an occupancy stage is present, carve each cell whose sampled field
//       value is below the threshold to empty (M18.5) — this is what lets a
//       recipe follow a surface instead of refining a solid cube,
//   (1) sample the material distribution into every (surviving) voxel,
//   (2) apply boundary overrides on the macro voxel's exposed faces
//       (overlap order bottom -> side -> top; top wins), then
//   (3) run each feature overlay in declared order over the filled grid.
//
// `macroChildMin` is the global child-layer VoxelCoord of the macro voxel's
// minimum corner, and `ratio` is how many child voxels span the macro voxel
// along each axis (chunkmath::layerRatio), so the worker can tell which voxels
// lie on which face without sampling any neighbor (§6/§13). `decompSeed` is the
// deterministic per-decomposition seed derived from (world_seed, macro coord).
//
// `gravityDir` is the "down" vector the top/bottom/side boundary roles are
// resolved against (M16, G2): the `top` distribution lands on the macro face most
// opposing gravity, `bottom` on the most-aligned face, and `side` on the lateral
// faces — so a decomposed crust is radial under a well instead of a flat +Y slab.
// The default constant -Y reproduces the historical +Y-top / -Y-bottom mapping
// byte-for-byte.
void fillChildChunk(Chunk& chunk, double voxelSizeM, const ResolvedRecipe& recipe,
                    chunkmath::VoxelCoord macroChildMin, int64_t ratio,
                    uint64_t decompSeed,
                    const glm::dvec3& gravityDir = glm::dvec3(0.0, -1.0, 0.0));

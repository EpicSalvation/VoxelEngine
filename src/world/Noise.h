#pragma once

#include <vector>

#include "plugin_api.h"  // NoiseFn, WorldCoord, RecipeParam

// Noise as a general engine facility (M9, docs/ARCHITECTURE.md §6).
//
// A noise function is a pure, deterministic scalar field sampled at a WORLD
// position, seeded by a uint64_t threaded through unchanged (no rand/time/global
// state). Sampling at the world position — not a macro-local one — makes adjacent
// macro voxels' child grids seamless, the same property the streaming heightmap
// relies on. Every built-in returns a scalar normalized to [0,1).
//
// These are the engine's first in-`src` noise. The engine registers them as the
// built-in floor at startup (PluginManager::registerBuiltinNoise, owner-tracked
// so a plugin unload never tears them down); a plugin register_noise of the same
// id OVERRIDES a built-in (the importer dispatch rule). In-tree consumers call
// these directly; the plugin-consume accessor (resolve_noise) is deferred to M13.
//
// Each function matches plugin_api.h's NoiseFn so the same pointer serves both
// in-tree callers and the noise registry. Common params (read via
// recipe_param_num): "scale" — feature size in world units (default 32); fbm /
// ridged also read "octaves", "lacunarity", "gain".
namespace noise {

float value(WorldCoord pos, uint64_t seed,
            const RecipeParam* params, size_t param_count, void* user_data);
float fbm(WorldCoord pos, uint64_t seed,
          const RecipeParam* params, size_t param_count, void* user_data);
float ridged(WorldCoord pos, uint64_t seed,
             const RecipeParam* params, size_t param_count, void* user_data);
float worley(WorldCoord pos, uint64_t seed,
             const RecipeParam* params, size_t param_count, void* user_data);

// One (id, fn) pair in the built-in set.
struct BuiltinNoise {
    const char* id;
    NoiseFn     fn;
};

// The full built-in set (value/fbm/ridged/worley). PluginManager iterates this to
// register the floor; tests iterate it to assert determinism.
const std::vector<BuiltinNoise>& builtins();

}  // namespace noise

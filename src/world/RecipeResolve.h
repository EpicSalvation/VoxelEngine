#pragma once

#include <vector>
#include "Recipe.h"
#include "ResolvedRecipe.h"

struct Recipe;
class PluginManager;

// Merge two param sets: `base` as the foundation, `override_params` winning on
// key collision. Keys absent from `base` but present in `override_params` are
// appended. Used by DecompositionManager::makeJob to build the inherited ancestor
// param set before passing it to resolveRecipe (ARCHITECTURE §6).
std::vector<RecipeParamValue> mergeRecipeParams(
    const std::vector<RecipeParamValue>& base,
    const std::vector<RecipeParamValue>& override_params);

// Resolve a Recipe's string references (material / noise / feature ids) into the
// concrete properties and function pointers a DecompositionWorker consumes
// (M9/M10, docs/ARCHITECTURE.md §6). This is the §13 boundary: it runs on the
// MAIN thread, consulting PluginManager once, so the worker never does.
//
// `inherited` is the merged seed_parameters of all ancestor recipes (root →
// immediate parent), reconstructed by DecompositionManager::makeJob by walking the
// ancestor coordinate+recipe chain (M10). For single-step / top-level recipes pass
// an empty vector — the recipe's own seed_parameters serve as the sole base.
//
// Merge precedence (weakest → strongest): inherited → this recipe's own
// seed_parameters → per-entry feature params (§6 "Hierarchical Constraints").
// Because the inherited set is a pure function of (world_seed, ancestor coords,
// recipes) it is re-derived identically after a clean evict, keeping the deep
// cache transparent.
//
// Material ids resolve through the M8 PluginManager::material lookup and fall
// back to the neutral default when unknown (fail-soft). An empty distribution
// noise_id resolves to the built-in "value". A feature/noise id with no
// registration resolves to a null fn — caught earlier by validateRecipes(), so
// a resolved recipe handed to the worker is already known-good.
ResolvedRecipe resolveRecipe(const Recipe& recipe, const PluginManager& pm,
                             const std::vector<RecipeParamValue>& inherited = {});

#pragma once

#include "ResolvedRecipe.h"

struct Recipe;
class PluginManager;

// Resolve a Recipe's string references (material / noise / feature ids) into the
// concrete properties and function pointers a DecompositionWorker consumes
// (M9, docs/ARCHITECTURE.md §6). This is the §13 boundary: it runs on the MAIN
// thread, consulting PluginManager once, so the worker never does.
//
// The recipe's own `seed_parameters` are merged into the effective parameter set
// of the distribution sampler and of every feature overlay; per-entry params win
// on a key collision (§6 "Hierarchical Constraints"). For M9 (single-step
// decomposition) the inherited parameters are simply the decomposing recipe's
// own seed_parameters — the cross-step cascade is M10.
//
// Material ids resolve through the M8 PluginManager::material lookup and fall
// back to the neutral default when unknown (fail-soft). An empty distribution
// noise_id resolves to the built-in "value". A feature/noise id with no
// registration resolves to a null fn — caught earlier by validateRecipes(), so
// a resolved recipe handed to the worker is already known-good.
ResolvedRecipe resolveRecipe(const Recipe& recipe, const PluginManager& pm);

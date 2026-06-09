#pragma once

class LayerConfig;
class PluginManager;

// Recipe validation, run at engine startup AFTER plugins are loaded (the second
// half of the §2 startup-validation contract — LayerConfig validates the stack
// shape before plugins load; this validates the recipes those plugins register).
//
// For every composite layer in the config:
//   - The layer resolves to a recipe: an explicitly registered one, or the
//     synthesized default recipe (the M6 run-the-child behavior) when none is
//     registered. A missing recipe is therefore NOT an error.
//   - A recipe that names a feature_generator_id or noise_id with no matching
//     registration is a hard startup error, not a silent skip (architecture.md §6).
//   - Material ids referenced by a distribution are NOT validated here: they
//     resolve through the M8 material lookup and fall back to the documented
//     neutral default, so an unknown id is fail-soft by design.
//
// Throws std::runtime_error with a descriptive message on the first problem, so
// the engine exits at startup rather than failing silently at runtime. Mirrors
// LayerConfig's throwing style.
void validateRecipes(const LayerConfig& config, const PluginManager& pm);

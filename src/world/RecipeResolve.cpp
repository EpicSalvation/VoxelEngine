#include "RecipeResolve.h"

#include <string>

#include "Recipe.h"
#include "core/PluginManager.h"

// Merge two param sets: `base` as the foundation, `override_params` winning on
// key collision. Keys absent from `base` but present in `override_params` are
// appended. Public so DecompositionManager::makeJob can build the inherited set.
std::vector<RecipeParamValue> mergeRecipeParams(
        const std::vector<RecipeParamValue>& base,
        const std::vector<RecipeParamValue>& override_params) {
    std::vector<RecipeParamValue> out = base;
    for (const RecipeParamValue& e : override_params) {
        bool replaced = false;
        for (RecipeParamValue& o : out) {
            if (o.key == e.key) { o = e; replaced = true; break; }
        }
        if (!replaced) out.push_back(e);
    }
    return out;
}

namespace {

ResolvedDistribution resolveDistribution(
        const DistributionValue& d, const PluginManager& pm,
        const std::vector<RecipeParamValue>& seedParams) {
    ResolvedDistribution rd;
    rd.materials.reserve(d.materials.size());
    for (const MaterialWeightValue& mw : d.materials)
        rd.materials.push_back({pm.material(mw.material_id), mw.weight});

    const std::string id = d.noise_id.empty() ? std::string("value") : d.noise_id;
    const RegisteredNoise* nz = pm.resolveNoise(id);
    rd.noise     = nz ? nz->fn : nullptr;
    rd.noiseUser = nz ? nz->user_data : nullptr;
    rd.noiseParams = mergeRecipeParams(seedParams, d.noise_params);
    return rd;
}

ResolvedBoundary resolveBoundary(const BoundaryValue& b, const PluginManager& pm,
                                 const std::vector<RecipeParamValue>& seedParams) {
    ResolvedBoundary rb;
    rb.present = b.present;
    rb.depth   = b.depth;
    if (b.present)
        rb.distribution = resolveDistribution(b.distribution, pm, seedParams);
    return rb;
}

}  // namespace

ResolvedRecipe resolveRecipe(const Recipe& recipe, const PluginManager& pm,
                             const std::vector<RecipeParamValue>& inherited) {
    // This recipe's own seed_parameters override the inherited (ancestor) set on
    // key collision; per-entry feature params win over both (§6 precedence).
    const std::vector<RecipeParamValue> seed =
        mergeRecipeParams(inherited, recipe.seed_parameters);

    ResolvedRecipe out;
    out.interior = resolveDistribution(recipe.interior, pm, seed);
    out.top      = resolveBoundary(recipe.top,    pm, seed);
    out.bottom   = resolveBoundary(recipe.bottom, pm, seed);
    out.side     = resolveBoundary(recipe.side,   pm, seed);

    out.features.reserve(recipe.features.size());
    for (size_t i = 0; i < recipe.features.size(); ++i) {
        const FeatureRefValue& f = recipe.features[i];
        ResolvedFeature rf;
        if (const RegisteredFeatureGenerator* g = pm.findFeatureGenerator(f.generator_id)) {
            rf.fn   = g->fn;
            rf.user = g->user_data;
        }
        rf.params   = mergeRecipeParams(seed, f.params);
        rf.seedSalt = static_cast<uint64_t>(i) + 1;  // 0 reserved for the distribution salt domain
        out.features.push_back(std::move(rf));
    }
    return out;
}

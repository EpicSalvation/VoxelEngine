#include "RecipeValidation.h"

#include <stdexcept>
#include <string>

#include "LayerConfig.h"
#include "PluginManager.h"
#include "world/Recipe.h"

namespace {

bool hasFeatureGenerator(const PluginManager& pm, const std::string& id) {
    for (const auto& g : pm.featureGenerators())
        if (g.generator_id == id)
            return true;
    return false;
}

// A distribution's noise_id (when non-empty) must resolve to a registered noise
// (built-in or plugin). An empty noise_id means the built-in "value" default.
void validateDistribution(const DistributionValue& dist, const PluginManager& pm,
                          const std::string& layerName, const std::string& where) {
    if (dist.noise_id.empty())
        return;
    if (!pm.resolveNoise(dist.noise_id))
        throw std::runtime_error(
            "Composite layer '" + layerName + "' recipe " + where +
            " references unregistered noise id '" + dist.noise_id +
            "'. Register it (built-in or via register_noise) or remove the reference.");
}

void validateRecipe(const Recipe& recipe, const PluginManager& pm,
                    const std::string& layerName) {
    // Material distributions: interior plus any present boundary overrides.
    validateDistribution(recipe.interior, pm, layerName, "interior distribution");
    if (recipe.top.present)
        validateDistribution(recipe.top.distribution, pm, layerName, "top boundary distribution");
    if (recipe.bottom.present)
        validateDistribution(recipe.bottom.distribution, pm, layerName, "bottom boundary distribution");
    if (recipe.side.present)
        validateDistribution(recipe.side.distribution, pm, layerName, "side boundary distribution");

    // Feature overlays must each name a registered feature generator.
    for (const FeatureRefValue& f : recipe.features) {
        if (!hasFeatureGenerator(pm, f.generator_id))
            throw std::runtime_error(
                "Composite layer '" + layerName + "' recipe references unregistered "
                "feature generator '" + f.generator_id +
                "'. Load the plugin that registers it or remove the overlay.");
    }
}

}  // namespace

void validateRecipes(const LayerConfig& config, const PluginManager& pm) {
    for (const LayerDef& layer : config.layers()) {
        if (layer.mode != VoxelMode::composite)
            continue;

        const Recipe* recipe = pm.findRecipe(layer.name);
        if (!recipe)
            continue;  // resolves to the synthesized default recipe — always valid

        validateRecipe(*recipe, pm, layer.name);
    }
}

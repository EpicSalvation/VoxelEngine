#pragma once

#include <string>
#include <vector>

#include "plugin_api.h"  // RecipeDesc family, RecipeParam, RecipeParamKind

// Internal composition-recipe value type (M9, docs/ARCHITECTURE.md §6).
//
// A plugin describes a recipe with the flat, POD RecipeDesc family in
// plugin_api.h (pointers + counts, so nothing std:: crosses the plugin ABI).
// At register_recipe time the engine DEEP-COPIES that description into the owning
// `Recipe` value type defined here, so the plugin's arrays need not outlive the
// call. Recipe owns its strings/vectors and depends on nothing beyond the public
// API and the standard library — no plugin/renderer/IO coupling (§13), so it is
// safe to store on PluginManager and to hand to DecompositionWorker.
//
// Material / feature / noise identifiers stay as STRINGS here. They resolve to
// MaterialProperties / FeatureGeneratorFn / NoiseFn only when a decomposition job
// is built on the main thread (§6), keeping the worker off PluginManager.

// Owning mirror of plugin_api.h's RecipeParam (a tagged key/value pair).
struct RecipeParamValue {
    std::string     key;
    RecipeParamKind kind   = RecipeParamKind::Number;
    double          number = 0.0;  // valid when kind == Number
    std::string     text;          // valid when kind == String (e.g. a material id)
};

// Owning mirror of MaterialWeight.
struct MaterialWeightValue {
    std::string material_id;
    float       weight = 0.0f;  // relative; normalized across the distribution
};

// Owning mirror of DistributionDesc: a weighted material set arranged spatially
// by a named noise field. An empty noise_id means the built-in "value" noise.
struct DistributionValue {
    std::vector<MaterialWeightValue> materials;
    std::string                      noise_id;      // empty => built-in "value"
    std::vector<RecipeParamValue>    noise_params;
};

// Owning mirror of FeatureRef: one ordered feature overlay with its params.
struct FeatureRefValue {
    std::string                   generator_id;
    std::vector<RecipeParamValue> params;
};

// Owning mirror of BoundaryDesc: a per-face override. `present == false` leaves
// the face to the interior distribution; `depth` is how many child-voxel layers
// inward from the face it replaces.
struct BoundaryValue {
    DistributionValue distribution;
    int               depth   = 1;
    bool              present = false;
    BoundaryMode      mode    = BoundaryMode::MacroFace;  // depth from macro face or carved surface (M18.5)
};

// Owning mirror of OccupancyDesc: the optional carve stage. `present == false`
// (the default) leaves the macro fully solid — exactly the pre-M18.5 behavior.
// `noise_id` empty means the built-in "value" noise.
struct OccupancyValue {
    std::string                   noise_id;            // empty => built-in "value"
    float                         threshold = 0.0f;
    std::vector<RecipeParamValue> params;
    bool                          present   = false;
};

struct Recipe {
    DistributionValue             interior;         // the bulk distribution
    std::vector<FeatureRefValue>  features;         // applied in declared order
    BoundaryValue                 top;              // overlap order at edges/corners:
    BoundaryValue                 bottom;           //   bottom -> side -> top (top wins)
    BoundaryValue                 side;             // shared by all four lateral faces
    std::vector<RecipeParamValue> seed_parameters;  // biases the layer below
    OccupancyValue                occupancy;         // optional carve stage (M18.5)

    // Deep-copy a flat POD RecipeDesc into an owning Recipe. Null id/text pointers
    // become empty strings; null arrays with a non-zero count are treated as empty.
    static Recipe fromDesc(const RecipeDesc& desc);
};

// ---------------------------------------------------------------------------
// Inline deep-copy helpers. Header-only so Recipe has no translation unit of its
// own (matching the other header-only value types under src/world/).
// ---------------------------------------------------------------------------
namespace recipe_detail {

inline std::string copyStr(const char* s) {
    return s ? std::string(s) : std::string();
}

inline RecipeParamValue copyParam(const RecipeParam& p) {
    RecipeParamValue v;
    v.key    = copyStr(p.key);
    v.kind   = p.kind;
    v.number = p.number;
    v.text   = copyStr(p.text);
    return v;
}

inline std::vector<RecipeParamValue> copyParams(const RecipeParam* params, size_t count) {
    std::vector<RecipeParamValue> out;
    if (!params) return out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i)
        out.push_back(copyParam(params[i]));
    return out;
}

inline DistributionValue copyDistribution(const DistributionDesc& d) {
    DistributionValue out;
    if (d.materials) {
        out.materials.reserve(d.material_count);
        for (size_t i = 0; i < d.material_count; ++i)
            out.materials.push_back({copyStr(d.materials[i].material_id),
                                     d.materials[i].weight});
    }
    out.noise_id     = copyStr(d.noise_id);
    out.noise_params = copyParams(d.noise_params, d.noise_param_count);
    return out;
}

inline BoundaryValue copyBoundary(const BoundaryDesc& b) {
    BoundaryValue out;
    out.distribution = copyDistribution(b.distribution);
    out.depth        = b.depth;
    out.present      = b.present;
    out.mode         = b.mode;
    return out;
}

inline OccupancyValue copyOccupancy(const OccupancyDesc& o) {
    OccupancyValue out;
    out.noise_id  = copyStr(o.noise_id);
    out.threshold = o.threshold;
    out.params    = copyParams(o.params, o.param_count);
    out.present   = o.present;
    return out;
}

}  // namespace recipe_detail

inline Recipe Recipe::fromDesc(const RecipeDesc& desc) {
    using namespace recipe_detail;
    Recipe r;
    r.interior = copyDistribution(desc.interior);
    if (desc.features) {
        r.features.reserve(desc.feature_count);
        for (size_t i = 0; i < desc.feature_count; ++i)
            r.features.push_back({copyStr(desc.features[i].generator_id),
                                  copyParams(desc.features[i].params,
                                             desc.features[i].param_count)});
    }
    r.top             = copyBoundary(desc.top);
    r.bottom          = copyBoundary(desc.bottom);
    r.side            = copyBoundary(desc.side);
    r.seed_parameters = copyParams(desc.seed_parameters, desc.seed_parameter_count);
    r.occupancy       = copyOccupancy(desc.occupancy);
    return r;
}

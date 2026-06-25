// Test fixture: a plugin that registers a material and then fails its init
// (non-zero return). The load must abort AND the partial registration must be
// rolled back, leaving no dangling entry in the registries.
#include "plugin_api.h"

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    MaterialProperties m;
    m.palette_index = 9;
    ctx->register_material(ctx, "fixture_badmat", m);  // registered before the failure
    return 7;                                          // non-zero: load must abort
}

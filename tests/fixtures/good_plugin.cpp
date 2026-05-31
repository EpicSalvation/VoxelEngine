// Test fixture: a well-formed plugin that registers one material and succeeds.
// Built as a MODULE shared library and loaded from disk by PluginManagerTest.
#include "plugin_api.h"

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    MaterialProperties m;
    m.density       = 1234.0f;
    m.palette_index = 7;
    ctx->register_material(ctx, "fixture_stone", m);
    return 0;
}

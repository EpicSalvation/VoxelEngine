// Test fixture: a plugin that exports voxel_plugin_init but stamps the wrong
// ABI version. Loading it must fail with an ABI-mismatch diagnostic.
#include "plugin_api.h"

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

VOXEL_PLUGIN_EXPORT const uint32_t voxel_plugin_abi_version = 9999;

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    (void)ctx;
    return 0;
}

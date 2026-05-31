// Test fixture: a valid shared library that deliberately does NOT export
// voxel_plugin_init. Loading it must fail with the missing-symbol error.
#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

VOXEL_PLUGIN_EXPORT int some_other_symbol() { return 0; }

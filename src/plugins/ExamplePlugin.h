#pragma once

#include "plugin_api.h"

// Reference plugin demonstrating material registration and layer generation.
//
// In a real deployment this would be compiled as a shared library (.so/.dylib/.dll)
// and loaded by PluginManager at runtime. For the engine executable it is wired in
// directly via PluginManager::wireInPlugin so the example runs without a .so build step.

extern "C" int voxel_plugin_init(PluginContext* ctx);

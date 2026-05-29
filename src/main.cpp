#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "plugins/ExamplePlugin.h"
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    // --- M1: validate layer configuration at startup ---
    // A bad config is a hard error: the engine exits before any world system initializes.
    LayerConfig layerConfig;
    try {
        layerConfig = LayerConfig::loadFromString(R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
)");
        std::cout << "[main] Layer config OK. "
                  << layerConfig.layers().size() << " layer(s) defined.\n";
    } catch (const std::exception& e) {
        std::cerr << "[main] Fatal: layer config error: " << e.what() << "\n";
        return 1;
    }

    // --- M4: load plugins ---
    // Scan the 'plugins' directory for .so/.dylib/.dll files, then wire in the
    // example plugin directly (it's compiled into this executable for development).
    PluginManager pluginManager;
    pluginManager.loadPluginsFromDirectory("plugins");
    pluginManager.wireInPlugin(voxel_plugin_init);

    std::cout << "[main] Registered materials: "
              << pluginManager.materials().size() << "\n";
    std::cout << "[main] Registered layer generators: "
              << pluginManager.layerGenerators().size() << "\n";

    // --- Engine startup ---
    Engine engine;
    engine.start();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    engine.stop();

    return 0;
}

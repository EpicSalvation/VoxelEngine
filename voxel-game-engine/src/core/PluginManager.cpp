#include "PluginManager.h"
#include <iostream>
#include <dlfcn.h>
#include <vector>
#include <filesystem>

PluginManager::PluginManager()
{
    // Set the extension based on the operating system
    std::string extension = ".so";
#ifdef _WIN32
    extension = ".dll";
#endif

    // Check working directory for a plugins foloder
    std::filesystem::path pluginsPath = std::filesystem::current_path() / "plugins";

    // Load all DLLs or SOs in the plugins folder
    if (std::filesystem::exists(pluginsPath) && std::filesystem::is_directory(pluginsPath))
    {
        std::cout << "Plugins folder found." << std::endl;
        for (const auto &entry : std::filesystem::directory_iterator(pluginsPath))
        {
            if (entry.path().extension() == ".so" || entry.path().extension() == ".dll")
            {
                loadPlugin(entry.path().string());
            }
        }
    }
    else
    {
        std::cout << "Plugins folder not found." << std::endl;
        return;
    }
}

PluginManager::~PluginManager()
{
    for (auto &plugin : plugins)
    {
        unloadPlugin(plugin);
    }
}

bool PluginManager::loadPlugin(const std::string &pluginPath)
{
    void *handle = dlopen(pluginPath.c_str(), RTLD_LAZY);
    if (!handle)
    {
        std::cerr << "Cannot load plugin: " << dlerror() << std::endl;
        return false;
    }

    Plugin *(*create)();
    create = (Plugin * (*)()) dlsym(handle, "create");
    const char *dlsym_error = dlerror();
    if (dlsym_error)
    {
        std::cerr << "Cannot load symbol create: " << dlsym_error << std::endl;
        dlclose(handle);
        return false;
    }

    Plugin *plugin = create();
    plugins.push_back(plugin);
    return true;
}

void PluginManager::unloadPlugin(Plugin *plugin)
{
    if (plugin)
    {
        plugin->cleanup();
        delete plugin;
    }
}

std::vector<Plugin *> PluginManager::getPlugins() const
{
    return plugins;
}
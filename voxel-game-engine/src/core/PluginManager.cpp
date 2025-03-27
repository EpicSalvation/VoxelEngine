#include "PluginManager.h"
#include <iostream>
#include <dlfcn.h>
#include <vector>
#include <filesystem>
#include "plugin_api.h"

PluginManager::PluginManager()
{
    // Set the extension based on the operating system
    std::string extension = ".so";
#ifdef _WIN32
    extension = ".dll";
#elif defined(__APPLE__)
    extension = ".dylib";
#endif

    // Check working directory for a plugins foloder
    std::filesystem::path pluginsPath = std::filesystem::current_path() / "plugins";

    // Load all DLLs or SOs in the plugins folder
    bool pluginsSuccessfullyLoaded = true;
    if (std::filesystem::exists(pluginsPath) && std::filesystem::is_directory(pluginsPath))
    {
        std::cout << "Plugins folder found." << std::endl;
        for (const auto &entry : std::filesystem::directory_iterator(pluginsPath))
        {
            if (entry.path().extension() == extension)
            {
                pluginsSuccessfullyLoaded &= loadPlugin(entry.path().string());
            }
        }
    }
    else
    {
        std::cout << "Plugins folder not found." << std::endl;
        return;
    }

    if (pluginsSuccessfullyLoaded)
    {
        std::cout << "All plugins loaded successfully." << std::endl;
    }
    else
    {
        std::cerr << "Some plugins failed to load." << std::endl;
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

    IPlugin *(*create)();
    create = (IPlugin * (*)()) dlsym(handle, "create");
    const char *dlsym_error = dlerror();
    if (dlsym_error)
    {
        std::cerr << "Cannot load symbol create: " << dlsym_error << std::endl;
        dlclose(handle);
        return false;
    }

    IPlugin *plugin = create();
    plugins.push_back(plugin);
    return true;
}

void PluginManager::unloadPlugin(IPlugin* pluginPtr)
{
    if (pluginPtr)
    {
        pluginPtr->cleanup();
        delete pluginPtr;
    }   
}

std::vector<IPlugin *> PluginManager::getPlugins() const
{
    return plugins;
}
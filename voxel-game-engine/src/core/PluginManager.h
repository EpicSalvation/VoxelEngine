#ifndef PLUGINMANAGER_H
#define PLUGINMANAGER_H

#include <vector>
#include <string>
#include "plugin_api.h"

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    bool loadPlugin(const std::string& pluginPath);
    void unloadPlugin(IPlugin* pluginPtr);
    std::vector<IPlugin *> getPlugins() const;

private:
    std::vector<IPlugin *> plugins;
};

#endif // PLUGINMANAGER_H
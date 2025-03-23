#ifndef PLUGINMANAGER_H
#define PLUGINMANAGER_H

#include <vector>
#include <string>

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    void loadPlugin(const std::string& pluginName);
    void unloadPlugin(const std::string& pluginName);
    std::vector<std::string> getPlugins() const;

private:
    std::vector<std::string> plugins;
};

#endif // PLUGINMANAGER_H
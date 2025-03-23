#ifndef PLUGIN_API_H
#define PLUGIN_API_H

class IPlugin {
public:
    virtual ~IPlugin() {}

    virtual void initialize() = 0;
    virtual void update() = 0;
    virtual void shutdown() = 0;
};

#endif // PLUGIN_API_H
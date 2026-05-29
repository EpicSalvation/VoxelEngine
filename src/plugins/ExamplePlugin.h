#ifndef EXAMPLEPLUGIN_H
#define EXAMPLEPLUGIN_H

#include "plugin_api.h"

class ExamplePlugin : public IPlugin {
public:
    void initialize() override;
    void update() override;
};

#endif // EXAMPLEPLUGIN_H
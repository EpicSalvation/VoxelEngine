#include "core/EngineConfig.h"

// The runtime budgets live in a single function-local static so there is exactly
// one instance per process and no static-init-order dependency. Read on the
// main-loop thread by each subsystem's tick; see EngineConfig.h.
EngineConfig& engineConfig() {
    static EngineConfig cfg;
    return cfg;
}

void resetEngineConfig() {
    engineConfig() = EngineConfig{};
}

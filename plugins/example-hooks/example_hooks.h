#pragma once

#include <cstdint>

// Shared observation state for the example-hooks plugin (M17).
//
// The plugin's callbacks record into this struct so a host or test can confirm
// each hook actually fired. A real plugin would keep its own richer state here
// and reach it through the `void* user_data` the engine threads back to every
// callback — which is exactly what this struct demonstrates: the plugin passes
// `&observed()` as the user_data for each registration, and every callback casts
// it back. The point is the WIRING, not the payload.
//
// observed() is an inline-static singleton so it resolves to ONE instance whether
// the plugin is compiled into the host/test binary (the path the test uses) or,
// in principle, called within the plugin's own address space — the same pattern
// the kinematic-body and input reference plugins use for their api() tables.
namespace example_hooks {

struct Observed {
    int   thermalEvents   = 0;
    int   lightingEvents  = 0;
    int   chunksCreated   = 0;
    int   chunksEvicted   = 0;
    int   interestChecks  = 0;
    float lastTemperature = 0.0f;
    float lastBrightness  = 0.0f;
};

inline Observed& observed() {
    static Observed s;
    return s;
}

}  // namespace example_hooks

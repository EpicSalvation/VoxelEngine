#pragma once

#include "plugin_api.h"

// A single voxel. Data is a MaterialProperties record, not a block type ID.
// See docs/ARCHITECTURE.md §5 for the design rationale.
struct Voxel {
    MaterialProperties material;

    // Returns an empty-space voxel (zero density, palette index 0).
    static Voxel empty() { return Voxel{}; }

    bool isEmpty() const {
        return material.density == 0.0f && material.palette_index == 0;
    }
};

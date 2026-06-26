#pragma once

#include <string>

#include "WorldCoord.h"

class Layer;

// Serialize a contiguous region of a layer to a Qubicle Binary .qb file.
//
// The region is written as a single matrix.  Unlike .vox, the .qb format has
// no 256-voxel-per-axis limit, so no auto-chunking is needed.
//
// Each voxel's palette_index is mapped to its RGBA color via the engine's
// shared visual palette (palette::color); empty voxels are written with
// alpha = 0.
//
// Returns true on success; prints to stderr on failure.
class QbExporter {
public:
    bool save(const std::string& path, const Layer& layer,
              const WorldCoord& minCorner, const WorldCoord& maxCorner) const;
};

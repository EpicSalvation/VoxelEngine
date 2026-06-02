#pragma once

#include <string>

#include "WorldCoord.h"

class Layer;

// Serialize a contiguous region of a layer to a MagicaVoxel .vox binary file.
//
// Regions ≤ 256 voxels in every axis are written as a single SIZE+XYZI object.
// Larger regions are auto-chunked: each 256³ sub-volume becomes a separate
// SIZE+XYZI object with an nTRN transform node encoding its world offset, so a
// compliant reader reassembles the original layout on re-import.
//
// Palette: used palette_index values get their built-in MagicaVoxel default
// color; indices not present in the region keep a neutral gray.
//
// Returns true on success; prints to stderr on failure.
class VoxExporter {
public:
    bool save(const std::string& path, const Layer& layer,
              const WorldCoord& minCorner, const WorldCoord& maxCorner) const;
};

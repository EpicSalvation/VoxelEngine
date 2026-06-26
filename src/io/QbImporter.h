#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "WorldCoord.h"

class Layer;
class PluginManager;

// ── Low-level .qb (Qubicle Binary) parse types ──────────────────────────────

namespace qb {

struct RgbaColor { uint8_t r, g, b, a; };

struct QbMatrix {
    std::string name;
    uint32_t    sizeX = 0, sizeY = 0, sizeZ = 0;
    int32_t     posX = 0, posY = 0, posZ = 0;
    // Dense voxel grid in z,y,x order: index = z*(sizeY*sizeX) + y*sizeX + x.
    // Alpha = 0 means empty.
    std::vector<RgbaColor> voxels;
};

struct QbFile {
    uint32_t version = 0;
    uint32_t colorFormat = 0;           // 0 = RGBA, 1 = BGRA
    uint32_t zAxisOrientation = 0;      // 0 = left-handed, 1 = right-handed
    uint32_t compressed = 0;            // 0 = uncompressed, 1 = RLE
    uint32_t visibilityMaskEncoded = 0; // 0 = no, 1 = yes
    std::vector<QbMatrix> matrices;
};

// Parse raw .qb bytes into a QbFile.  Returns false on truncated / malformed
// data.  Handles both uncompressed and RLE-compressed matrices, and both
// RGBA and BGRA color formats (normalised to RGBA on output).
bool parse(const uint8_t* data, size_t size, QbFile& out);

}  // namespace qb

// ── High-level import interface ──────────────────────────────────────────────

// Reads a .qb file from disk and places its voxels into `layer` at `anchor`.
//
// The .qb format stores raw RGBA colors per voxel rather than palette indices.
// On import, unique colors are mapped to palette indices 1..255 and installed
// into the engine's shared visual palette (src/renderer/Palette.h).  If the
// file contains more than 255 unique colors a warning is logged and excess
// colors are dropped.
//
// Material properties for each palette_index are resolved via PluginManager
// (architecture.md SS10 -- properties come from the registry, never from the
// color value).
//
// Returns true on success; logs to stderr on failure.
class QbImporter {
public:
    bool load(const std::string& path, Layer& layer, const WorldCoord& anchor,
              const PluginManager& plugins);
};

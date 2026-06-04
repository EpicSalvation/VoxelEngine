#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "WorldCoord.h"

class Layer;
class PluginManager;

// ── Low-level .vox parse types ────────────────────────────────────────────────

namespace vox {

struct RgbaColor { uint8_t r, g, b, a; };

// One voxel entry from a XYZI chunk: local position + 1-based color index.
// Color index 0 means empty in the .vox convention and is skipped on import.
struct VoxEntry { uint8_t x, y, z, colorIndex; };

// One parsed model (SIZE + XYZI pair) and its world-space translation from nTRN.
// The offset is the translation from the nTRN transform node that references this
// model's nSHP; zero for files without a scene graph.
struct VoxModel {
    uint32_t              sizeX = 0, sizeY = 0, sizeZ = 0;
    std::vector<VoxEntry> voxels;
    int32_t               offsetX = 0, offsetY = 0, offsetZ = 0;
};

// Fully parsed .vox file.  palette[0] is unused by the .vox convention (index 0
// = empty); entries 1–255 map to RGBA colors.  When the file has no RGBA chunk
// the built-in MagicaVoxel default palette is used instead.
struct VoxFile {
    std::vector<VoxModel>      models;
    std::array<RgbaColor, 256> palette{};
};

// Parse raw .vox bytes into a VoxFile.  Returns false on bad magic / truncated
// data.  Fills out.palette with the built-in default palette when the file
// contains no RGBA chunk.
bool parse(const uint8_t* data, size_t size, VoxFile& out);

}  // namespace vox

// ── High-level import interface ───────────────────────────────────────────────

// Reads a .vox file from disk and places its voxels into `layer` at `anchor`.
//
// Each placed voxel receives:
//   - palette_index  set directly from the .vox color index
//   - other MaterialProperties fields  copied from the first PluginManager
//     material whose palette_index matches; zero-defaults if none is registered
//     (architecture.md §10 — no inference from color values)
//
// The file's authored RGBA colors are also installed into the engine's shared
// visual palette (src/renderer/Palette.h) for the indices the model uses, so the
// imported voxels render with — and re-export to — their original colors.
//
// Chunks that do not yet exist in the layer are created as empty before voxels
// are written; all written chunks are marked dirty so they are persisted.
// Returns true on success; logs to stderr on failure.
class VoxImporter {
public:
    bool load(const std::string& path, Layer& layer, const WorldCoord& anchor,
              const PluginManager& plugins);
};

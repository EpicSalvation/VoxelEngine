#pragma once

#include <cstdint>
#include <vector>

// Headless CPU-side texture-atlas packer (M15 T3). Deliberately bgfx-free so it
// can be unit-tested without a GPU context — the same split ChunkMeshData uses
// against ChunkMesh. TextureManager decodes images (via bimg) into RGBA8 tiles,
// hands them here to be packed into one atlas image, then uploads the result
// through bgfx::createTexture2D and exposes each tile's UV rect.
//
// Packing is a deterministic shelf (row) packer: tiles are placed left-to-right
// in insertion order, wrapping to a new shelf when the current row fills. The
// atlas width is the next power of two that holds the widest tile and a target
// width; the height grows to the next power of two that holds all shelves. Tiles
// may be any resolution (a 128×128 hand-painted block beside 16×16 terrain
// tiles), per the audit's design note.

namespace texture {

// A tile's normalized UV sub-rect within the atlas, plus its source pixel size.
// (u0,v0) is the top-left corner, (u1,v1) the bottom-right. The mesh builder
// (T5) addresses a face into [u0,u1]×[v0,v1]; the in-tile repeat wrap also lives
// against this rect.
struct AtlasTile {
    float    u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    uint16_t w  = 0,    h  = 0;   // source tile pixel dimensions
};

class TextureAtlasData {
public:
    // Add an RGBA8 tile (row-major, 4 bytes/pixel, w*h*4 bytes). The pixels are
    // copied, so the caller's buffer need not outlive the call. Returns the tile
    // index (the order tiles are added), used to look up its UV rect after pack().
    // A zero-area tile is rejected (returns -1).
    int addTile(const uint8_t* rgba, uint16_t w, uint16_t h);

    // Pack every added tile into a single atlas image. targetWidth nudges the
    // atlas width (it is raised to fit the widest tile and rounded up to a power
    // of two). Safe to call once after all tiles are added; idempotent inputs
    // produce a byte-identical atlas (deterministic layout).
    void pack(uint16_t targetWidth = 256);

    // Atlas image, valid after pack(). pixels() is width()*height()*4 bytes of
    // RGBA8, or empty when no tiles were added.
    uint16_t                    width()  const { return width_; }
    uint16_t                    height() const { return height_; }
    const std::vector<uint8_t>& pixels() const { return pixels_; }

    size_t           tileCount()    const { return tiles_.size(); }
    const AtlasTile& tile(size_t i) const { return tiles_[i]; }

private:
    struct Source { std::vector<uint8_t> rgba; uint16_t w, h; };
    std::vector<Source>    sources_;
    std::vector<AtlasTile> tiles_;
    std::vector<uint8_t>   pixels_;
    uint16_t               width_  = 0;
    uint16_t               height_ = 0;
};

}  // namespace texture

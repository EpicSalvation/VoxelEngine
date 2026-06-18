#pragma once

#include <cstdint>
#include <string>

#include "TextureAtlasData.h"  // texture::AtlasTile (UV sub-rect)

// (palette_index, face) → atlas tile binding (M15 T4).
//
// The material-keyed counterpart to `palette` (Palette.h): where the palette maps
// a material's palette_index to one solid color, this maps it to a per-face atlas
// tile so a "grass block" shows a grass tile on top and dirt on the sides. The
// table is keyed by the palette_index every voxel already carries — NO field is
// added to Voxel / MaterialProperties, so the POD, the determinism padding, RLE
// persistence (§9), and the plugin ABI are all untouched (the audit's decision A).
//
// Like set_palette_color, set_material_faces writes a global runtime table and is
// NOT owner-tracked: a binding persists, but its visual effect tracks the atlas.
// A bound texture_id resolves to a tile only while that texture is registered, so
// when a register_texture owner unloads and the atlas rebuilds (TextureManager),
// the binding falls back to the white tile automatically — the §8 teardown end
// state without a second teardown path.
//
// Two tables, resolved at query time so the mesh builder stays headless:
//   1. bindings  (palette_index, face) → texture_id + tiling_factor  [set_material_faces]
//   2. tile rects texture_id → AtlasTile                              [TextureManager::rebuild]
// The mesh builder (T5) calls faceTile() which joins them; tests populate both
// directly with no GPU context.
namespace materialfaces {

// Face direction index — MUST match kFaces[] order in ChunkMeshData.cpp:
//   0:+Z  1:-Z  2:-Y (bottom)  3:+Y (top)  4:-X  5:+X
enum Face : int {
    PosZ = 0, NegZ = 1, NegY = 2, PosY = 3, NegX = 4, PosX = 5,
    kFaceCount = 6
};

// Resolved per-face tile handed to the mesh builder. When `bound` is false the
// builder emits the white-tile UVs so the per-face color renders unmodulated.
struct FaceTile {
    texture::AtlasTile tile;                 // atlas sub-rect addressed by the face
    float              tiling_factor = 1.0f; // tiles per world meter (T5 repeat scale)
    bool               bound         = false;
};

// Bind a material's faces to registered texture ids. The four lateral faces share
// `side`; `top` is +Y and `bottom` is -Y. A null or empty id leaves that face
// unbound (white). tiling_factor (tiles per world meter) is carried per material,
// so the same texture is scale-agnostic across a 1 m terminal voxel and a large
// composite block (T5 scales the emitted UV span by face_world_size × factor).
void setMaterialFaces(uint8_t     palette_index,
                      const char* top,
                      const char* bottom,
                      const char* side,
                      float       tiling_factor);

// Install/replace a texture id's resolved atlas sub-rect. Called by
// TextureManager after each atlas pack so bindings resolve to live UVs; tests
// call it directly to drive faceTile() without a GPU atlas.
void setTileRect(const std::string& texture_id, const texture::AtlasTile& tile);

// Drop all resolved tile rects (TextureManager calls this at the start of a
// rebuild before re-installing the survivors; an empty set ⇒ every binding falls
// back to white).
void clearTiles();

// Drop all material-face bindings (test/reset helper).
void clearBindings();

// Resolve a material face to its tile. `bound` is false when the material has no
// binding for this face OR the bound texture id has no atlas rect yet — both mean
// "render the white tile" to the builder.
FaceTile faceTile(uint8_t palette_index, int faceIndex);

}  // namespace materialfaces

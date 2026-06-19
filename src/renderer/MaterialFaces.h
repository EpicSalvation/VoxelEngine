#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

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

// Bind a material's faces by ROLE — `top` is the up-facing skin, `bottom` the
// down-facing one, and `side` is shared by all lateral faces. The four lateral
// slots take `side`; the up slot takes `top` and the down slot `bottom`. The
// authoring frame is constant -Y gravity (up = +Y), so a binding made here is
// resolved against the live gravity vector at query time (faceTile): under the
// default -Y this is the historical +Y-top / -Y-bottom mapping byte-for-byte,
// but under a radial well the same `top` tile renders on whichever geometric
// face is "up" — grass-side-out on an asteroid (M16 G1, axisrole::roleOf).
// A null or empty id leaves that role unbound (white). tiling_factor (tiles per
// world meter) is carried per material, so the same texture is scale-agnostic
// across a 1 m terminal voxel and a large composite block (T5 scales the emitted
// UV span by face_world_size × factor).
void setMaterialFaces(uint8_t     palette_index,
                      const char* top,
                      const char* bottom,
                      const char* side,
                      float       tiling_factor);

// Bind all six geometric faces explicitly, in Face-enum order
// (+Z,-Z,-Y,+Y,-X,+X) — the six-independent-faces form (M16 G1). Like
// setMaterialFaces the bindings are authored in the constant -Y frame and
// resolved against the live gravity vector at query time, but every face gets
// its own tile rather than the four laterals sharing one. A null/empty id leaves
// that face unbound (white).
void setMaterialFacesAll(uint8_t     palette_index,
                         const char* posZ,
                         const char* negZ,
                         const char* negY,
                         const char* posY,
                         const char* negX,
                         const char* posX,
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

// Resolve a material face to its tile against a gravity ("down") vector. The
// face's gravity-relative role (axisrole::roleOf) selects which authored slot —
// up / down / lateral — supplies the tile, so the up-facing geometric face
// always shows the `top` binding regardless of which way is "down". `bound` is
// false when the resolved role has no binding OR the bound texture id has no
// atlas rect yet — both mean "render the white tile" to the builder.
FaceTile faceTile(uint8_t palette_index, int faceIndex, const glm::dvec3& gravityDir);

// Convenience overload resolving against the engine-default constant -Y gravity
// (up = +Y) — the historical Y-up mapping, byte-identical to pre-M16 callers.
FaceTile faceTile(uint8_t palette_index, int faceIndex);

}  // namespace materialfaces

#include "MaterialFaces.h"

#include <array>
#include <unordered_map>

#include "world/AxisRole.h"

namespace materialfaces {
namespace {

// Outward unit normal of each face, in kFaces order (must match the Face enum
// and ChunkMeshData.cpp's kFaces[]): 0:+Z 1:-Z 2:-Y 3:+Y 4:-X 5:+X.
constexpr glm::dvec3 kFaceNormals[kFaceCount] = {
    {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}, {0.0, -1.0, 0.0},
    {0.0, 1.0, 0.0}, {-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
};

// Resolve a geometric face to the canonical binding slot that supplies its tile
// under `gravityDir`. Bindings are authored in the constant -Y frame (up = +Y
// slot, down = -Y slot, the four lateral slots hold `side`), so:
//   * an Up-role face reads the +Y (top) slot,
//   * a Down-role face reads the -Y (bottom) slot,
//   * a Lateral-role face reads its own slot when it is itself a lateral face
//     (preserving six-independent bindings under the default), or a canonical
//     lateral slot when the dominant axis rotated a former top/bottom face into
//     a side.
// Under default -Y this is the identity, so every face reads its own slot and
// the mesh is byte-identical to the pre-M16 +Y-top mapping.
int canonicalSlot(int faceIndex, const glm::dvec3& gravityDir) {
    const axisrole::Role r = axisrole::roleOf(kFaceNormals[faceIndex], gravityDir);
    if (r == axisrole::Role::Up)   return PosY;
    if (r == axisrole::Role::Down) return NegY;
    // Lateral: keep the face's own slot unless it is a +Y/-Y face that rotated
    // sideways, in which case fall back to a canonical lateral slot.
    if (faceIndex == PosY || faceIndex == NegY) return PosZ;
    return faceIndex;
}

// One material's per-face bindings. A null/empty texture_id is stored as empty
// (present == false) so faceTile() short-circuits to the white fallback.
struct Binding {
    std::array<std::string, kFaceCount> texture_id;  // by Face index
    float                               tiling_factor = 1.0f;
};

// Runtime tables, one shared instance across translation units (mirrors
// palette::detail::g_table). Indexed by palette_index (256 materials).
struct Tables {
    std::array<Binding, 256>                   bindings;
    std::unordered_map<std::string, texture::AtlasTile> tiles;  // texture_id → rect
};
Tables& tables() {
    static Tables t;
    return t;
}

// Set one face slot, treating null/empty as "leave unbound".
void setFace(Binding& b, int face, const char* id) {
    b.texture_id[static_cast<size_t>(face)] = (id && id[0]) ? id : std::string();
}

}  // namespace

void setMaterialFaces(uint8_t palette_index, const char* top, const char* bottom,
                      const char* side, float tiling_factor) {
    Binding& b = tables().bindings[palette_index];
    setFace(b, PosY, top);     // +Y
    setFace(b, NegY, bottom);  // -Y
    // The four lateral faces share the single `side` texture.
    setFace(b, PosZ, side);
    setFace(b, NegZ, side);
    setFace(b, NegX, side);
    setFace(b, PosX, side);
    b.tiling_factor = (tiling_factor > 0.0f) ? tiling_factor : 1.0f;
}

void setMaterialFacesAll(uint8_t palette_index, const char* posZ, const char* negZ,
                         const char* negY, const char* posY, const char* negX,
                         const char* posX, float tiling_factor) {
    Binding& b = tables().bindings[palette_index];
    setFace(b, PosZ, posZ);
    setFace(b, NegZ, negZ);
    setFace(b, NegY, negY);
    setFace(b, PosY, posY);
    setFace(b, NegX, negX);
    setFace(b, PosX, posX);
    b.tiling_factor = (tiling_factor > 0.0f) ? tiling_factor : 1.0f;
}

void setTileRect(const std::string& texture_id, const texture::AtlasTile& tile) {
    tables().tiles[texture_id] = tile;
}

void clearTiles() {
    tables().tiles.clear();
}

void clearBindings() {
    for (Binding& b : tables().bindings) {
        for (std::string& id : b.texture_id) id.clear();
        b.tiling_factor = 1.0f;
    }
}

FaceTile faceTile(uint8_t palette_index, int faceIndex, const glm::dvec3& gravityDir) {
    FaceTile out;
    if (faceIndex < 0 || faceIndex >= kFaceCount) return out;  // unbound

    const int          slot = canonicalSlot(faceIndex, gravityDir);
    const Binding&     b    = tables().bindings[palette_index];
    const std::string& id   = b.texture_id[static_cast<size_t>(slot)];
    if (id.empty()) return out;  // no binding for this role → white

    auto it = tables().tiles.find(id);
    if (it == tables().tiles.end()) return out;  // texture not (yet) in the atlas → white

    out.tile          = it->second;
    out.tiling_factor = b.tiling_factor;
    out.bound         = true;
    return out;
}

FaceTile faceTile(uint8_t palette_index, int faceIndex) {
    // Engine-default constant -Y gravity (up = +Y): the historical Y-up mapping.
    return faceTile(palette_index, faceIndex, glm::dvec3(0.0, -1.0, 0.0));
}

}  // namespace materialfaces

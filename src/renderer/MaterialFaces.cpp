#include "MaterialFaces.h"

#include <array>
#include <unordered_map>

namespace materialfaces {
namespace {

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

FaceTile faceTile(uint8_t palette_index, int faceIndex) {
    FaceTile out;
    if (faceIndex < 0 || faceIndex >= kFaceCount) return out;  // unbound

    const Binding&     b  = tables().bindings[palette_index];
    const std::string& id = b.texture_id[static_cast<size_t>(faceIndex)];
    if (id.empty()) return out;  // no binding for this face → white

    auto it = tables().tiles.find(id);
    if (it == tables().tiles.end()) return out;  // texture not (yet) in the atlas → white

    out.tile          = it->second;
    out.tiling_factor = b.tiling_factor;
    out.bound         = true;
    return out;
}

}  // namespace materialfaces

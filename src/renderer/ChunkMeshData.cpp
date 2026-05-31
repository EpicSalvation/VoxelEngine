#include "ChunkMeshData.h"
#include "Palette.h"

namespace {

// The eight corners of a unit voxel as 0/1 offsets, indexed to match the cube
// template winding in BgfxRenderer (so chunk meshes and per-voxel cubes agree on
// front/back faces). Faces are wound CCW-outward; the renderer culls CCW (the
// interior) for opaque geometry — see the kOpaqueState note in BgfxRenderer.cpp.
constexpr int kCorner[8][3] = {
    {0, 0, 1},  // 0
    {1, 0, 1},  // 1
    {1, 1, 1},  // 2
    {0, 1, 1},  // 3
    {0, 0, 0},  // 4
    {1, 0, 0},  // 5
    {1, 1, 0},  // 6
    {0, 1, 0},  // 7
};

struct Face {
    int   dx, dy, dz;  // neighbor direction; face is drawn when that neighbor is empty
    int   tri[6];      // two triangles (six corner indices), template winding
    float shade;       // per-face brightness, baked into vertex color (see below)
};

// Fixed directional shading (no real lighting): top faces full brightness, sides
// dimmer, bottom darkest, with N/S (Z) and E/W (X) at different levels so all
// four sides are distinguishable. This gives flat-colored terrain enough contrast
// to read its shape. Values follow the familiar Minecraft-style ramp.
constexpr Face kFaces[6] = {
    {  0,  0,  1, {0, 1, 2, 0, 2, 3}, 0.8f },  // +Z
    {  0,  0, -1, {4, 6, 5, 4, 7, 6}, 0.8f },  // -Z
    {  0, -1,  0, {0, 4, 5, 0, 5, 1}, 0.5f },  // -Y (bottom)
    {  0,  1,  0, {2, 6, 7, 2, 7, 3}, 1.0f },  // +Y (top)
    { -1,  0,  0, {0, 3, 7, 0, 7, 4}, 0.6f },  // -X
    {  1,  0,  0, {1, 5, 6, 1, 6, 2}, 0.6f },  // +X
};

bool inBounds(int n, int x, int y, int z) {
    return x >= 0 && x < n && y >= 0 && y < n && z >= 0 && z < n;
}

// Scale the RGB channels of an ABGR color by `f`, leaving alpha unchanged.
uint32_t shadeColor(uint32_t abgr, float f) {
    auto ch = [f](uint32_t c) -> uint32_t {
        const float v = static_cast<float>(c) * f;
        return static_cast<uint32_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    };
    const uint32_t r = ch(abgr & 0xff);
    const uint32_t g = ch((abgr >> 8) & 0xff);
    const uint32_t b = ch((abgr >> 16) & 0xff);
    const uint32_t a = (abgr >> 24) & 0xff;
    return r | (g << 8) | (b << 16) | (a << 24);
}

}  // namespace

void buildChunkMeshData(const Chunk& chunk,
                        std::vector<MeshVertex>& out_vertices,
                        std::vector<uint32_t>&   out_opaque_indices,
                        std::vector<uint32_t>&   out_translucent_indices) {
    out_vertices.clear();
    out_opaque_indices.clear();
    out_translucent_indices.clear();

    const int n = chunk.size();
    for (int z = 0; z < n; ++z) {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const Voxel& v = chunk.at(x, y, z);
                if (v.isEmpty()) continue;

                const uint32_t color       = palette::color(v.material.palette_index);
                const bool     translucent = palette::isTranslucent(v.material.palette_index);
                std::vector<uint32_t>& indices =
                    translucent ? out_translucent_indices : out_opaque_indices;

                for (const Face& f : kFaces) {
                    const int nx = x + f.dx, ny = y + f.dy, nz = z + f.dz;

                    if (inBounds(n, nx, ny, nz)) {
                        // Cull this face only if the in-chunk neighbor actually
                        // occludes it. An opaque neighbor hides anything behind it.
                        // A translucent neighbor (water) hides another translucent
                        // face (water interior) but NOT an opaque face — a grass or
                        // stone top sitting under water must still render and show
                        // through it. So: water voxels cull against any non-empty
                        // neighbor; opaque voxels cull only against an opaque one.
                        const Voxel& nb = chunk.at(nx, ny, nz);
                        if (!nb.isEmpty()) {
                            const bool nbTranslucent =
                                palette::isTranslucent(nb.material.palette_index);
                            if (translucent || !nbTranslucent)
                                continue;
                        }
                    } else {
                        // Border: opaque voxels emit the face (no cross-chunk
                        // lookup); translucent voxels assume the medium continues
                        // and skip it, so water does not wall up at chunk seams.
                        if (translucent)
                            continue;
                    }

                    // Bake directional shading into opaque face colors so flat-
                    // colored terrain reads its shape (top brightest, sides/bottom
                    // darker). Translucent media (water) keep a uniform color —
                    // per-face shading on a flat fluid surface looks wrong.
                    const uint32_t faceColor =
                        translucent ? color : shadeColor(color, f.shade);
                    for (int i = 0; i < 6; ++i) {
                        const int* c = kCorner[f.tri[i]];
                        indices.push_back(static_cast<uint32_t>(out_vertices.size()));
                        out_vertices.push_back(MeshVertex{
                            static_cast<float>(x + c[0]),
                            static_cast<float>(y + c[1]),
                            static_cast<float>(z + c[2]),
                            faceColor,
                        });
                    }
                }
            }
        }
    }
}

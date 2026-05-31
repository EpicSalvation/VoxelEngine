#include "ChunkMeshData.h"
#include "Palette.h"

namespace {

// The eight corners of a unit voxel as 0/1 offsets, indexed to match the cube
// template winding in BgfxRenderer (so chunk meshes and per-voxel cubes agree
// on front/back faces under BGFX_STATE_DEFAULT's CW culling).
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
    int dx, dy, dz;    // neighbor direction; face is drawn when that neighbor is empty
    int tri[6];        // two triangles (six corner indices), template winding
};

constexpr Face kFaces[6] = {
    {  0,  0,  1, {0, 1, 2, 0, 2, 3} },  // +Z
    {  0,  0, -1, {4, 6, 5, 4, 7, 6} },  // -Z
    {  0, -1,  0, {0, 4, 5, 0, 5, 1} },  // -Y
    {  0,  1,  0, {2, 6, 7, 2, 7, 3} },  // +Y
    { -1,  0,  0, {0, 3, 7, 0, 7, 4} },  // -X
    {  1,  0,  0, {1, 5, 6, 1, 6, 2} },  // +X
};

bool inBounds(int n, int x, int y, int z) {
    return x >= 0 && x < n && y >= 0 && y < n && z >= 0 && z < n;
}

bool solidAt(const Chunk& chunk, int x, int y, int z) {
    return !chunk.at(x, y, z).isEmpty();
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
                        // Cull against a solid in-chunk neighbor.
                        if (solidAt(chunk, nx, ny, nz))
                            continue;
                    } else {
                        // Border: opaque voxels emit the face (no cross-chunk
                        // lookup); translucent voxels assume the medium continues
                        // and skip it, so water does not wall up at chunk seams.
                        if (translucent)
                            continue;
                    }

                    for (int i = 0; i < 6; ++i) {
                        const int* c = kCorner[f.tri[i]];
                        indices.push_back(static_cast<uint32_t>(out_vertices.size()));
                        out_vertices.push_back(MeshVertex{
                            static_cast<float>(x + c[0]),
                            static_cast<float>(y + c[1]),
                            static_cast<float>(z + c[2]),
                            color,
                        });
                    }
                }
            }
        }
    }
}

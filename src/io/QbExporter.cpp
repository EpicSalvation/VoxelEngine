#include "QbExporter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "renderer/Palette.h"
#include "world/Layer.h"
#include "world/ChunkCoordMath.h"
#include "world/Voxel.h"

// ── Binary write helpers ─────────────────────────────────────────────────────

namespace {

void writeU8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

void writeU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back( v        & 0xFF);
    buf.push_back((v >>  8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

void writeI32(std::vector<uint8_t>& buf, int32_t v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    writeU32(buf, u);
}

}  // anonymous namespace

// ── QbExporter::save ─────────────────────────────────────────────────────────

bool QbExporter::save(const std::string& path, const Layer& layer,
                      const WorldCoord& minCorner,
                      const WorldCoord& maxCorner) const {
    const double vsm = layer.voxelSizeM();
    const int    csv = layer.chunkSizeVoxels();

    const chunkmath::VoxelCoord minVC = chunkmath::worldToVoxel(minCorner, vsm);
    const chunkmath::VoxelCoord maxVC = chunkmath::worldToVoxel(maxCorner, vsm);

    const int64_t totalX = maxVC.x - minVC.x;
    const int64_t totalY = maxVC.y - minVC.y;
    const int64_t totalZ = maxVC.z - minVC.z;

    if (totalX <= 0 || totalY <= 0 || totalZ <= 0) {
        std::fprintf(stderr, "QbExporter: empty region for '%s'\n", path.c_str());
        return false;
    }

    // Build a dense RGBA grid for the export region.
    // .qb stores colors directly (not palette indices), so we convert each
    // voxel's palette_index to RGBA via the engine's visual palette.
    const size_t total = static_cast<size_t>(totalX) * totalY * totalZ;
    std::vector<uint32_t> grid(total, 0);  // 0 = empty (alpha = 0)

    for (const auto& [cc, chunk] : layer.chunks()) {
        const int64_t cMinX = static_cast<int64_t>(cc.x) * csv;
        const int64_t cMinY = static_cast<int64_t>(cc.y) * csv;
        const int64_t cMinZ = static_cast<int64_t>(cc.z) * csv;
        const int64_t cMaxX = cMinX + csv;
        const int64_t cMaxY = cMinY + csv;
        const int64_t cMaxZ = cMinZ + csv;

        if (cMaxX <= minVC.x || cMinX >= maxVC.x) continue;
        if (cMaxY <= minVC.y || cMinY >= maxVC.y) continue;
        if (cMaxZ <= minVC.z || cMinZ >= maxVC.z) continue;

        const int64_t oMinX = std::max(cMinX, minVC.x);
        const int64_t oMinY = std::max(cMinY, minVC.y);
        const int64_t oMinZ = std::max(cMinZ, minVC.z);
        const int64_t oMaxX = std::min(cMaxX, maxVC.x);
        const int64_t oMaxY = std::min(cMaxY, maxVC.y);
        const int64_t oMaxZ = std::min(cMaxZ, maxVC.z);

        for (int64_t gz = oMinZ; gz < oMaxZ; ++gz) {
            for (int64_t gy = oMinY; gy < oMaxY; ++gy) {
                for (int64_t gx = oMinX; gx < oMaxX; ++gx) {
                    const Voxel& v = chunk->at(
                        static_cast<int>(gx - cMinX),
                        static_cast<int>(gy - cMinY),
                        static_cast<int>(gz - cMinZ));
                    if (v.isEmpty()) continue;

                    // palette::color returns ABGR (0xAABBGGRR).
                    // .qb RGBA byte order = R,G,B,A = little-endian 0xAABBGGRR.
                    // The bit layout is identical, so we can write it directly.
                    const uint32_t abgr = palette::color(v.material.palette_index);

                    const size_t rx = static_cast<size_t>(gx - minVC.x);
                    const size_t ry = static_cast<size_t>(gy - minVC.y);
                    const size_t rz = static_cast<size_t>(gz - minVC.z);
                    const size_t idx = rz * (totalY * totalX) + ry * totalX + rx;
                    grid[idx] = abgr;
                }
            }
        }
    }

    // ── Assemble .qb binary ──────────────────────────────────────────────────
    std::vector<uint8_t> out;

    // Header
    writeU32(out, 0x00000101);  // version 1.1.0.0
    writeU32(out, 0);           // colorFormat = RGBA
    writeU32(out, 1);           // zAxisOrientation = right-handed
    writeU32(out, 0);           // compressed = uncompressed
    writeU32(out, 0);           // visibilityMaskEncoded = no
    writeU32(out, 1);           // numMatrices = 1

    // Matrix header
    const std::string matName = "export";
    writeU8(out, static_cast<uint8_t>(matName.size()));
    for (char c : matName) writeU8(out, static_cast<uint8_t>(c));
    writeU32(out, static_cast<uint32_t>(totalX));
    writeU32(out, static_cast<uint32_t>(totalY));
    writeU32(out, static_cast<uint32_t>(totalZ));
    writeI32(out, 0);  // posX
    writeI32(out, 0);  // posY
    writeI32(out, 0);  // posZ

    // Voxel data (uncompressed, z,y,x order)
    for (int64_t z = 0; z < totalZ; ++z) {
        for (int64_t y = 0; y < totalY; ++y) {
            for (int64_t x = 0; x < totalX; ++x) {
                const size_t idx = static_cast<size_t>(z) * (totalY * totalX)
                                 + static_cast<size_t>(y) * totalX + x;
                writeU32(out, grid[idx]);
            }
        }
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "QbExporter: cannot open '%s' for writing\n",
                     path.c_str());
        return false;
    }
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    if (!file) {
        std::fprintf(stderr, "QbExporter: write error '%s'\n", path.c_str());
        return false;
    }
    return true;
}

#include "VoxExporter.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "VoxImporter.h"          // vox::RgbaColor
#include "world/Layer.h"
#include "world/ChunkCoordMath.h"
#include "world/Voxel.h"

// Built-in MagicaVoxel default palette (same table as VoxImporter.cpp).
// Used to assign reasonable colors to exported palette indices.
static const vox::RgbaColor kDefaultPalette[256] = {
    {  0,   0,   0,   0}, // 0 = empty/unused
    {255, 255, 255, 255}, {255, 255, 204, 255}, {255, 255, 153, 255}, {255, 255, 102, 255},
    {255, 255,  51, 255}, {255, 255,   0, 255}, {255, 204, 255, 255}, {255, 204, 204, 255},
    {255, 204, 153, 255}, {255, 204, 102, 255}, {255, 204,  51, 255}, {255, 204,   0, 255},
    {255, 153, 255, 255}, {255, 153, 204, 255}, {255, 153, 153, 255}, {255, 153, 102, 255},
    {255, 153,  51, 255}, {255, 153,   0, 255}, {255, 102, 255, 255}, {255, 102, 204, 255},
    {255, 102, 153, 255}, {255, 102, 102, 255}, {255, 102,  51, 255}, {255, 102,   0, 255},
    {255,  51, 255, 255}, {255,  51, 204, 255}, {255,  51, 153, 255}, {255,  51, 102, 255},
    {255,  51,  51, 255}, {255,  51,   0, 255}, {255,   0, 255, 255}, {255,   0, 204, 255},
    {255,   0, 153, 255}, {255,   0, 102, 255}, {255,   0,  51, 255}, {255,   0,   0, 255},
    {204, 255, 255, 255}, {204, 255, 204, 255}, {204, 255, 153, 255}, {204, 255, 102, 255},
    {204, 255,  51, 255}, {204, 255,   0, 255}, {204, 204, 255, 255}, {204, 204, 204, 255},
    {204, 204, 153, 255}, {204, 204, 102, 255}, {204, 204,  51, 255}, {204, 204,   0, 255},
    {204, 153, 255, 255}, {204, 153, 204, 255}, {204, 153, 153, 255}, {204, 153, 102, 255},
    {204, 153,  51, 255}, {204, 153,   0, 255}, {204, 102, 255, 255}, {204, 102, 204, 255},
    {204, 102, 153, 255}, {204, 102, 102, 255}, {204, 102,  51, 255}, {204, 102,   0, 255},
    {204,  51, 255, 255}, {204,  51, 204, 255}, {204,  51, 153, 255}, {204,  51, 102, 255},
    {204,  51,  51, 255}, {204,  51,   0, 255}, {204,   0, 255, 255}, {204,   0, 204, 255},
    {204,   0, 153, 255}, {204,   0, 102, 255}, {204,   0,  51, 255}, {204,   0,   0, 255},
    {153, 255, 255, 255}, {153, 255, 204, 255}, {153, 255, 153, 255}, {153, 255, 102, 255},
    {153, 255,  51, 255}, {153, 255,   0, 255}, {153, 204, 255, 255}, {153, 204, 204, 255},
    {153, 204, 153, 255}, {153, 204, 102, 255}, {153, 204,  51, 255}, {153, 204,   0, 255},
    {153, 153, 255, 255}, {153, 153, 204, 255}, {153, 153, 153, 255}, {153, 153, 102, 255},
    {153, 153,  51, 255}, {153, 153,   0, 255}, {153, 102, 255, 255}, {153, 102, 204, 255},
    {153, 102, 153, 255}, {153, 102, 102, 255}, {153, 102,  51, 255}, {153, 102,   0, 255},
    {153,  51, 255, 255}, {153,  51, 204, 255}, {153,  51, 153, 255}, {153,  51, 102, 255},
    {153,  51,  51, 255}, {153,  51,   0, 255}, {153,   0, 255, 255}, {153,   0, 204, 255},
    {153,   0, 153, 255}, {153,   0, 102, 255}, {153,   0,  51, 255}, {153,   0,   0, 255},
    {102, 255, 255, 255}, {102, 255, 204, 255}, {102, 255, 153, 255}, {102, 255, 102, 255},
    {102, 255,  51, 255}, {102, 255,   0, 255}, {102, 204, 255, 255}, {102, 204, 204, 255},
    {102, 204, 153, 255}, {102, 204, 102, 255}, {102, 204,  51, 255}, {102, 204,   0, 255},
    {102, 153, 255, 255}, {102, 153, 204, 255}, {102, 153, 153, 255}, {102, 153, 102, 255},
    {102, 153,  51, 255}, {102, 153,   0, 255}, {102, 102, 255, 255}, {102, 102, 204, 255},
    {102, 102, 153, 255}, {102, 102, 102, 255}, {102, 102,  51, 255}, {102, 102,   0, 255},
    {102,  51, 255, 255}, {102,  51, 204, 255}, {102,  51, 153, 255}, {102,  51, 102, 255},
    {102,  51,  51, 255}, {102,  51,   0, 255}, {102,   0, 255, 255}, {102,   0, 204, 255},
    {102,   0, 153, 255}, {102,   0, 102, 255}, {102,   0,  51, 255}, {102,   0,   0, 255},
    { 51, 255, 255, 255}, { 51, 255, 204, 255}, { 51, 255, 153, 255}, { 51, 255, 102, 255},
    { 51, 255,  51, 255}, { 51, 255,   0, 255}, { 51, 204, 255, 255}, { 51, 204, 204, 255},
    { 51, 204, 153, 255}, { 51, 204, 102, 255}, { 51, 204,  51, 255}, { 51, 204,   0, 255},
    { 51, 153, 255, 255}, { 51, 153, 204, 255}, { 51, 153, 153, 255}, { 51, 153, 102, 255},
    { 51, 153,  51, 255}, { 51, 153,   0, 255}, { 51, 102, 255, 255}, { 51, 102, 204, 255},
    { 51, 102, 153, 255}, { 51, 102, 102, 255}, { 51, 102,  51, 255}, { 51, 102,   0, 255},
    { 51,  51, 255, 255}, { 51,  51, 204, 255}, { 51,  51, 153, 255}, { 51,  51, 102, 255},
    { 51,  51,  51, 255}, { 51,  51,   0, 255}, { 51,   0, 255, 255}, { 51,   0, 204, 255},
    { 51,   0, 153, 255}, { 51,   0, 102, 255}, { 51,   0,  51, 255}, { 51,   0,   0, 255},
    {  0, 255, 255, 255}, {  0, 255, 204, 255}, {  0, 255, 153, 255}, {  0, 255, 102, 255},
    {  0, 255,  51, 255}, {  0, 255,   0, 255}, {  0, 204, 255, 255}, {  0, 204, 204, 255},
    {  0, 204, 153, 255}, {  0, 204, 102, 255}, {  0, 204,  51, 255}, {  0, 204,   0, 255},
    {  0, 153, 255, 255}, {  0, 153, 204, 255}, {  0, 153, 153, 255}, {  0, 153, 102, 255},
    {  0, 153,  51, 255}, {  0, 153,   0, 255}, {  0, 102, 255, 255}, {  0, 102, 204, 255},
    {  0, 102, 153, 255}, {  0, 102, 102, 255}, {  0, 102,  51, 255}, {  0, 102,   0, 255},
    {  0,  51, 255, 255}, {  0,  51, 204, 255}, {  0,  51, 153, 255}, {  0,  51, 102, 255},
    {  0,  51,  51, 255}, {  0,  51,   0, 255}, {  0,   0, 255, 255}, {  0,   0, 204, 255},
    {  0,   0, 153, 255}, {  0,   0, 102, 255}, {  0,   0,  51, 255}, {100, 100, 100, 255},
    {150, 150, 150, 255}, {200, 200, 200, 255},
};

// ── Binary write helpers ──────────────────────────────────────────────────────

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

void writeStr(std::vector<uint8_t>& buf, const std::string& s) {
    writeI32(buf, static_cast<int32_t>(s.size()));
    for (char c : s) writeU8(buf, static_cast<uint8_t>(c));
}

// Append a chunk: 4-byte id, content_size, children_size, content, children.
void appendChunk(std::vector<uint8_t>& out, const char* id,
                 const std::vector<uint8_t>& content,
                 const std::vector<uint8_t>& children = {}) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(id[i]));
    writeU32(out, static_cast<uint32_t>(content.size()));
    writeU32(out, static_cast<uint32_t>(children.size()));
    out.insert(out.end(), content.begin(), content.end());
    out.insert(out.end(), children.begin(), children.end());
}

std::vector<uint8_t> makeSizeContent(uint32_t sx, uint32_t sy, uint32_t sz) {
    std::vector<uint8_t> c;
    writeU32(c, sx); writeU32(c, sy); writeU32(c, sz);
    return c;
}

std::vector<uint8_t> makeXyziContent(
        const std::vector<std::array<uint8_t, 4>>& voxels) {
    std::vector<uint8_t> c;
    writeU32(c, static_cast<uint32_t>(voxels.size()));
    for (const auto& v : voxels) {
        writeU8(c, v[0]); writeU8(c, v[1]); writeU8(c, v[2]); writeU8(c, v[3]);
    }
    return c;
}

// 256 RGBA entries: palette[1..255] → file slots 0..254; file slot 255 = gray.
// Used palette indices get their default MagicaVoxel color; unused get gray.
std::vector<uint8_t> makeRgbaContent(const bool usedIndices[256]) {
    std::vector<uint8_t> c;
    c.reserve(1024);
    for (int i = 1; i <= 255; ++i) {
        if (usedIndices[i]) {
            writeU8(c, kDefaultPalette[i].r);
            writeU8(c, kDefaultPalette[i].g);
            writeU8(c, kDefaultPalette[i].b);
            writeU8(c, kDefaultPalette[i].a);
        } else {
            writeU8(c, 125); writeU8(c, 125); writeU8(c, 125); writeU8(c, 255);
        }
    }
    // Slot 255 (last entry in the RGBA chunk): unused by convention, write gray.
    writeU8(c, 125); writeU8(c, 125); writeU8(c, 125); writeU8(c, 255);
    return c;
}

// nTRN: node_id, empty attr dict, child_id, reserved=-1, layer_id=-1,
//       num_frames=1, frame0 dict with _t = "tx ty tz".
std::vector<uint8_t> makeTrnContent(int32_t nodeId, int32_t childId,
                                    int32_t tx, int32_t ty, int32_t tz) {
    std::vector<uint8_t> c;
    writeI32(c, nodeId);
    writeI32(c, 0);       // empty attr dict
    writeI32(c, childId);
    writeI32(c, -1);      // reserved
    writeI32(c, -1);      // layer_id
    writeI32(c, 1);       // num_frames
    // frame 0: one-entry dict {_t: "tx ty tz"}
    std::ostringstream oss;
    oss << tx << " " << ty << " " << tz;
    writeI32(c, 1);
    writeStr(c, "_t");
    writeStr(c, oss.str());
    return c;
}

// nGRP: node_id, empty attr dict, num_children, child_ids[].
std::vector<uint8_t> makeGrpContent(int32_t nodeId,
                                    const std::vector<int32_t>& childIds) {
    std::vector<uint8_t> c;
    writeI32(c, nodeId);
    writeI32(c, 0);  // empty attr dict
    writeI32(c, static_cast<int32_t>(childIds.size()));
    for (int32_t id : childIds) writeI32(c, id);
    return c;
}

// nSHP: node_id, empty attr dict, num_models=1, model_id, empty model attr dict.
std::vector<uint8_t> makeShpContent(int32_t nodeId, int32_t modelId) {
    std::vector<uint8_t> c;
    writeI32(c, nodeId);
    writeI32(c, 0);  // empty attr dict
    writeI32(c, 1);  // num_models
    writeI32(c, modelId);
    writeI32(c, 0);  // empty model attr dict
    return c;
}

// A single model's data collected from the export region.
struct Model {
    uint32_t sizeX = 0, sizeY = 0, sizeZ = 0;
    int32_t  offsetX = 0, offsetY = 0, offsetZ = 0;  // nTRN translation
    std::vector<std::array<uint8_t, 4>> voxels;       // {lx, ly, lz, colorIdx}
};

}  // anonymous namespace

// ── VoxExporter::save ─────────────────────────────────────────────────────────

bool VoxExporter::save(const std::string& path, const Layer& layer,
                       const WorldCoord& minCorner,
                       const WorldCoord& maxCorner) const {
    const double vsm = layer.voxelSizeM();
    const int    csv = layer.chunkSizeVoxels();

    // Convert world bounds to global voxel coords (inclusive min, exclusive max).
    const chunkmath::VoxelCoord minVC = chunkmath::worldToVoxel(minCorner, vsm);
    const chunkmath::VoxelCoord maxVC = chunkmath::worldToVoxel(maxCorner, vsm);

    const int64_t totalX = maxVC.x - minVC.x;
    const int64_t totalY = maxVC.y - minVC.y;
    const int64_t totalZ = maxVC.z - minVC.z;

    if (totalX <= 0 || totalY <= 0 || totalZ <= 0) {
        std::fprintf(stderr, "VoxExporter: empty region for '%s'\n", path.c_str());
        return false;
    }

    // Collect all non-empty voxels from resident chunks that overlap the region.
    // Each entry is {rel_x, rel_y, rel_z, palette_index} relative to minVC.
    struct RelVoxel { int64_t x, y, z; uint8_t idx; };
    std::vector<RelVoxel> allVoxels;

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
                    allVoxels.push_back(
                        {gx - minVC.x, gy - minVC.y, gz - minVC.z,
                         v.material.palette_index});
                }
            }
        }
    }

    // Build the used-index set for palette construction.
    bool usedIndices[256] = {};
    for (const auto& rv : allVoxels)
        usedIndices[rv.idx] = true;

    // ── Partition into sub-volumes (256³ max per .vox object) ────────────────
    const int64_t kMax = 256;
    const int64_t nx = (totalX + kMax - 1) / kMax;
    const int64_t ny = (totalY + kMax - 1) / kMax;
    const int64_t nz = (totalZ + kMax - 1) / kMax;
    const int64_t nModels = nx * ny * nz;

    std::vector<Model> models;
    models.resize(static_cast<size_t>(nModels));

    int64_t mi = 0;
    for (int64_t iz = 0; iz < nz; ++iz) {
        for (int64_t iy = 0; iy < ny; ++iy) {
            for (int64_t ix = 0; ix < nx; ++ix, ++mi) {
                const int64_t subMinX = ix * kMax;
                const int64_t subMinY = iy * kMax;
                const int64_t subMinZ = iz * kMax;
                const int64_t subSzX = std::min(kMax, totalX - subMinX);
                const int64_t subSzY = std::min(kMax, totalY - subMinY);
                const int64_t subSzZ = std::min(kMax, totalZ - subMinZ);

                Model& m = models[static_cast<size_t>(mi)];
                m.sizeX = static_cast<uint32_t>(subSzX);
                m.sizeY = static_cast<uint32_t>(subSzY);
                m.sizeZ = static_cast<uint32_t>(subSzZ);
                // nTRN offset = sub-volume min (relative to export anchor)
                //               + half size (the .vox model-center convention).
                m.offsetX = static_cast<int32_t>(subMinX + subSzX / 2);
                m.offsetY = static_cast<int32_t>(subMinY + subSzY / 2);
                m.offsetZ = static_cast<int32_t>(subMinZ + subSzZ / 2);
            }
        }
    }

    // Distribute voxels into their sub-volume.
    for (const auto& rv : allVoxels) {
        const int64_t ix2 = rv.x / kMax;
        const int64_t iy2 = rv.y / kMax;
        const int64_t iz2 = rv.z / kMax;
        const int64_t mi2 = iz2 * ny * nx + iy2 * nx + ix2;
        Model& m = models[static_cast<size_t>(mi2)];
        const int64_t subMinX = ix2 * kMax;
        const int64_t subMinY = iy2 * kMax;
        const int64_t subMinZ = iz2 * kMax;
        const uint8_t lx = static_cast<uint8_t>(rv.x - subMinX);
        const uint8_t ly = static_cast<uint8_t>(rv.y - subMinY);
        const uint8_t lz = static_cast<uint8_t>(rv.z - subMinZ);
        m.voxels.push_back({lx, ly, lz, rv.idx});
    }

    // ── Assemble .vox binary ──────────────────────────────────────────────────
    std::vector<uint8_t> children;

    // SIZE + XYZI chunks for every model.
    for (const auto& m : models) {
        appendChunk(children, "SIZE", makeSizeContent(m.sizeX, m.sizeY, m.sizeZ));
        appendChunk(children, "XYZI", makeXyziContent(m.voxels));
    }

    // RGBA palette chunk.
    appendChunk(children, "RGBA", makeRgbaContent(usedIndices));

    // Scene graph (always emitted so the nTRN center offsets are applied correctly
    // on re-import, even for single-model files).
    // Layout: nTRN(0)→nGRP(1)→[nTRN(2k)→nSHP(2k+1)] for each model k.
    appendChunk(children, "nTRN", makeTrnContent(0, 1, 0, 0, 0));

    std::vector<int32_t> grpChildren;
    for (int64_t k = 0; k < nModels; ++k)
        grpChildren.push_back(static_cast<int32_t>(2 + k * 2));
    appendChunk(children, "nGRP", makeGrpContent(1, grpChildren));

    for (int64_t k = 0; k < nModels; ++k) {
        const Model& m = models[static_cast<size_t>(k)];
        const int32_t trnId = static_cast<int32_t>(2 + k * 2);
        const int32_t shpId = trnId + 1;
        appendChunk(children, "nTRN",
                    makeTrnContent(trnId, shpId,
                                   m.offsetX, m.offsetY, m.offsetZ));
        appendChunk(children, "nSHP",
                    makeShpContent(shpId, static_cast<int32_t>(k)));
    }

    // Wrap everything in a MAIN chunk.
    std::vector<uint8_t> file;
    file.push_back('V'); file.push_back('O');
    file.push_back('X'); file.push_back(' ');
    writeU32(file, 200);  // version
    appendChunk(file, "MAIN", {}, children);

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "VoxExporter: cannot open '%s' for writing\n",
                     path.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
    if (!out) {
        std::fprintf(stderr, "VoxExporter: write error '%s'\n", path.c_str());
        return false;
    }
    return true;
}

#include "VoxImporter.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "world/Layer.h"
#include "world/ChunkCoordMath.h"
#include "core/PluginManager.h"
#include "renderer/Palette.h"
#include "world/Voxel.h"

// ── MagicaVoxel default palette (indices 1–255; index 0 = empty) ─────────────
// Source: MagicaVoxel 0.99 default palette, widely documented and public domain.
// Values are stored RGBA (r, g, b, a) matching the VoxFile::palette layout.

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

// ── Binary reader helpers ─────────────────────────────────────────────────────

namespace {

struct Reader {
    const uint8_t* data;
    size_t         size;
    size_t         pos = 0;

    bool eof() const { return pos >= size; }
    bool canRead(size_t n) const { return pos + n <= size; }

    bool readU8(uint8_t& v) {
        if (!canRead(1)) return false;
        v = data[pos++];
        return true;
    }

    bool readU32(uint32_t& v) {
        if (!canRead(4)) return false;
        std::memcpy(&v, data + pos, 4);
        pos += 4;
        return true;
    }

    bool readI32(int32_t& v) {
        uint32_t u; if (!readU32(u)) return false;
        std::memcpy(&v, &u, 4);
        return true;
    }

    // Read a length-prefixed string (int32 length + bytes).
    bool readString(std::string& s) {
        int32_t len;
        if (!readI32(len) || len < 0) return false;
        if (!canRead(static_cast<size_t>(len))) return false;
        s.assign(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(len));
        pos += static_cast<size_t>(len);
        return true;
    }

    // Skip a DICT (n key-value string pairs).
    bool skipDict() {
        int32_t n; if (!readI32(n) || n < 0) return false;
        for (int32_t i = 0; i < n; ++i) {
            std::string k, v;
            if (!readString(k) || !readString(v)) return false;
        }
        return true;
    }

    // Read a DICT and extract one string value by key. Returns default if missing.
    bool readDict(std::string& foundKey, std::string& foundVal,
                  const std::string& wantKey) {
        int32_t n; if (!readI32(n) || n < 0) return false;
        for (int32_t i = 0; i < n; ++i) {
            std::string k, v;
            if (!readString(k) || !readString(v)) return false;
            if (k == wantKey) { foundKey = k; foundVal = v; }
        }
        return true;
    }

    bool skip(size_t n) {
        if (!canRead(n)) return false;
        pos += n;
        return true;
    }
};

// Parse chunk header: id (4 bytes), content size (u32), children size (u32).
struct ChunkHeader {
    char     id[5] = {};
    uint32_t contentSize = 0;
    uint32_t childrenSize = 0;
};

bool readChunkHeader(Reader& r, ChunkHeader& h) {
    for (int i = 0; i < 4; ++i)
        if (!r.readU8(reinterpret_cast<uint8_t&>(h.id[i]))) return false;
    h.id[4] = '\0';
    return r.readU32(h.contentSize) && r.readU32(h.childrenSize);
}

bool idEq(const char* a, const char* b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

// Parse a translation string "_t" value like "1 2 3" into three ints.
bool parseTranslation(const std::string& s, int32_t& x, int32_t& y, int32_t& z) {
    return std::sscanf(s.c_str(), "%d %d %d", &x, &y, &z) == 3;
}

}  // anonymous namespace

// ── vox::parse ────────────────────────────────────────────────────────────────

namespace vox {

bool parse(const uint8_t* data, size_t size, VoxFile& out) {
    Reader r{data, size};

    // Magic "VOX " + version (u32, ignored for compatibility)
    char magic[4];
    for (int i = 0; i < 4; ++i) {
        uint8_t b; if (!r.readU8(b)) return false;
        magic[i] = static_cast<char>(b);
    }
    if (magic[0]!='V' || magic[1]!='O' || magic[2]!='X' || magic[3]!=' ')
        return false;
    uint32_t version; if (!r.readU32(version)) return false;
    (void)version;

    // MAIN chunk header
    ChunkHeader mainHdr;
    if (!readChunkHeader(r, mainHdr)) return false;
    if (!idEq(mainHdr.id, "MAIN")) return false;

    // Remaining bytes belong to MAIN's children
    size_t childrenEnd = r.pos + mainHdr.childrenSize;
    if (childrenEnd > r.size) return false;

    // --- First pass: collect SIZE, XYZI, RGBA in order ---
    // We need to handle the case where nTRN/nSHP nodes interleave with models,
    // so we store all chunk offsets first then do a second pass for scene graph.

    struct RawChunk { std::string id; size_t contentStart; uint32_t contentSize; };
    std::vector<RawChunk> chunks;

    {
        Reader scan{data, size};
        scan.pos = r.pos;
        while (scan.pos < childrenEnd) {
            ChunkHeader h;
            if (!readChunkHeader(scan, h)) break;
            chunks.push_back({std::string(h.id), scan.pos, h.contentSize});
            // skip content + children
            if (!scan.skip(h.contentSize + h.childrenSize)) break;
        }
    }

    // Fill default palette first; overwrite if RGBA chunk is present.
    for (int i = 0; i < 256; ++i) out.palette[i] = kDefaultPalette[i];

    // --- Pass 1: SIZE → XYZI pairs and RGBA ---
    // SIZE and XYZI always appear as consecutive pairs in order.
    {
        vox::VoxModel* pendingModel = nullptr;
        for (const auto& c : chunks) {
            if (c.id == "SIZE") {
                out.models.emplace_back();
                pendingModel = &out.models.back();
                Reader cr{data, size}; cr.pos = c.contentStart;
                cr.readU32(pendingModel->sizeX);
                cr.readU32(pendingModel->sizeY);
                cr.readU32(pendingModel->sizeZ);
            } else if (c.id == "XYZI" && pendingModel) {
                Reader cr{data, size}; cr.pos = c.contentStart;
                uint32_t n; if (!cr.readU32(n)) continue;
                pendingModel->voxels.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    uint8_t x, y, z, idx;
                    if (!cr.readU8(x) || !cr.readU8(y) || !cr.readU8(z) || !cr.readU8(idx))
                        break;
                    if (idx != 0)
                        pendingModel->voxels.push_back({x, y, z, idx});
                }
                pendingModel = nullptr;
            } else if (c.id == "RGBA") {
                Reader cr{data, size}; cr.pos = c.contentStart;
                // RGBA chunk stores 256 entries; index i in file = palette[i+1]
                // because .vox stores palette starting at index 1 in slot 0.
                for (int i = 0; i < 255 && cr.canRead(4); ++i) {
                    uint8_t rv, gv, bv, av;
                    cr.readU8(rv); cr.readU8(gv); cr.readU8(bv); cr.readU8(av);
                    out.palette[i + 1] = {rv, gv, bv, av};
                }
                // slot 255 in file → palette[256] which is out of range; skip last
                if (cr.canRead(4)) { uint32_t dummy; cr.readU32(dummy); }
            }
        }
    }

    if (out.models.empty()) {
        // Version 150 files may have no scene graph and just a single model pair.
        // Already handled above; if still empty, file has no voxel data.
        return true;
    }

    // --- Pass 2: nTRN and nSHP for per-model translation offsets ---
    // Build map: shape_node_id → model_index
    // Build map: nTRN_node_id → {child_node_id, translation}
    struct TrnInfo { int32_t childId; int32_t tx, ty, tz; };
    std::unordered_map<int32_t, TrnInfo> trnMap;
    std::unordered_map<int32_t, int32_t> shpToModel;  // shape_node_id → model idx

    for (const auto& c : chunks) {
        if (c.id == "nSHP") {
            Reader cr{data, size}; cr.pos = c.contentStart;
            int32_t nodeId; if (!cr.readI32(nodeId)) continue;
            if (!cr.skipDict()) continue;
            int32_t numModels; if (!cr.readI32(numModels)) continue;
            // First model id of this shape
            int32_t modelId; if (!cr.readI32(modelId)) continue;
            if (modelId >= 0 && modelId < static_cast<int32_t>(out.models.size()))
                shpToModel[nodeId] = modelId;
        } else if (c.id == "nTRN") {
            Reader cr{data, size}; cr.pos = c.contentStart;
            int32_t nodeId; if (!cr.readI32(nodeId)) continue;
            if (!cr.skipDict()) continue;
            int32_t childId; if (!cr.readI32(childId)) continue;
            int32_t reserved; if (!cr.readI32(reserved)) continue;
            int32_t layerId;  if (!cr.readI32(layerId))  continue;
            int32_t numFrames; if (!cr.readI32(numFrames)) continue;

            int32_t tx = 0, ty = 0, tz = 0;
            bool framesOk = true;
            for (int32_t f = 0; f < numFrames && framesOk; ++f) {
                int32_t n; if (!cr.readI32(n) || n < 0) { framesOk = false; break; }
                for (int32_t j = 0; j < n && framesOk; ++j) {
                    std::string k, v;
                    if (!cr.readString(k) || !cr.readString(v)) { framesOk = false; break; }
                    if (k == "_t") parseTranslation(v, tx, ty, tz);
                }
            }
            trnMap[nodeId] = {childId, tx, ty, tz};
        }
    }

    // Walk nTRN nodes: those whose child is a nSHP node assign translation to model.
    for (const auto& [nodeId, trn] : trnMap) {
        auto it = shpToModel.find(trn.childId);
        if (it != shpToModel.end()) {
            int32_t midx = it->second;
            if (midx >= 0 && midx < static_cast<int32_t>(out.models.size())) {
                out.models[midx].offsetX = trn.tx;
                out.models[midx].offsetY = trn.ty;
                out.models[midx].offsetZ = trn.tz;
            }
        }
    }

    return true;
}

}  // namespace vox

// ── VoxImporter::load ─────────────────────────────────────────────────────────

bool VoxImporter::load(const std::string& path, Layer& layer,
                       const WorldCoord& anchor, const PluginManager& plugins) {
    // Read file
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "VoxImporter: cannot open '%s'\n", path.c_str());
        return false;
    }
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        std::fprintf(stderr, "VoxImporter: read error '%s'\n", path.c_str());
        return false;
    }

    // Parse
    vox::VoxFile file;
    if (!vox::parse(buf.data(), buf.size(), file)) {
        std::fprintf(stderr, "VoxImporter: parse error '%s'\n", path.c_str());
        return false;
    }

    // Install the file's authored colors into the engine's visual palette for
    // every color index this model uses, so imported voxels render with — and
    // re-export to — the colors they were drawn with. The engine palette is
    // index-based and shared (src/renderer/Palette.h); we touch only the indices
    // this file references, leaving other entries at their engine defaults. This
    // is the visual layer only; material *properties* are still taken from
    // registered materials below, not inferred from color (architecture.md §10).
    for (const auto& model : file.models) {
        for (const auto& ve : model.voxels) {
            if (ve.colorIndex == 0) continue;
            const vox::RgbaColor& rc = file.palette[ve.colorIndex];
            const uint32_t abgr = static_cast<uint32_t>(rc.r)
                                | (static_cast<uint32_t>(rc.g) << 8)
                                | (static_cast<uint32_t>(rc.b) << 16)
                                | (static_cast<uint32_t>(rc.a) << 24);
            palette::setColor(ve.colorIndex, abgr);
        }
    }

    // Build palette_index → MaterialProperties lookup from registered materials.
    // Index 0 stays as zero-default (empty voxels are skipped anyway).
    std::array<MaterialProperties, 256> propsByPalette{};
    for (int i = 1; i < 256; ++i) {
        propsByPalette[i].palette_index = static_cast<uint8_t>(i);
    }
    for (const auto& mat : plugins.materials()) {
        uint8_t idx = mat.props.palette_index;
        if (idx != 0)
            propsByPalette[idx] = mat.props;
    }

    // Pre-create any chunks that will be needed so setVoxel can find them.
    // We collect all required ChunkCoords before any writes.
    const double vsm = layer.voxelSizeM();
    const int    csv = layer.chunkSizeVoxels();

    std::unordered_set<ChunkCoord, ChunkCoordHash> needed;
    for (const auto& model : file.models) {
        const int32_t halfX = static_cast<int32_t>(model.sizeX) / 2;
        const int32_t halfY = static_cast<int32_t>(model.sizeY) / 2;
        const int32_t halfZ = static_cast<int32_t>(model.sizeZ) / 2;

        for (const auto& ve : model.voxels) {
            const double wx = anchor.value.x +
                static_cast<double>(model.offsetX + static_cast<int32_t>(ve.x) - halfX) * vsm;
            const double wy = anchor.value.y +
                static_cast<double>(model.offsetY + static_cast<int32_t>(ve.y) - halfY) * vsm;
            const double wz = anchor.value.z +
                static_cast<double>(model.offsetZ + static_cast<int32_t>(ve.z) - halfZ) * vsm;
            WorldCoord wc(wx, wy, wz);
            needed.insert(chunkmath::worldToChunk(wc, vsm, csv));
        }
    }

    for (const ChunkCoord& cc : needed)
        layer.loadChunk(cc, nullptr);  // creates empty chunk if not already resident

    // Write voxels
    for (const auto& model : file.models) {
        const int32_t halfX = static_cast<int32_t>(model.sizeX) / 2;
        const int32_t halfY = static_cast<int32_t>(model.sizeY) / 2;
        const int32_t halfZ = static_cast<int32_t>(model.sizeZ) / 2;

        for (const auto& ve : model.voxels) {
            if (ve.colorIndex == 0) continue;

            const double wx = anchor.value.x +
                static_cast<double>(model.offsetX + static_cast<int32_t>(ve.x) - halfX) * vsm;
            const double wy = anchor.value.y +
                static_cast<double>(model.offsetY + static_cast<int32_t>(ve.y) - halfY) * vsm;
            const double wz = anchor.value.z +
                static_cast<double>(model.offsetZ + static_cast<int32_t>(ve.z) - halfZ) * vsm;

            Voxel v;
            v.material = propsByPalette[ve.colorIndex];
            layer.setVoxel(WorldCoord(wx, wy, wz), v);
        }
    }

    return true;
}

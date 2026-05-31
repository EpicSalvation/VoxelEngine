#include "ChunkPersistence.h"

#include "world/World.h"
#include "world/ChunkCoordMath.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace persistence {
namespace {

// File format constants. Little-endian; floats are IEEE-754. This is an internal
// save format for the platforms the engine targets (all little-endian), not a
// portable interchange format — that role belongs to the M7 .vox/.vxe path.
constexpr char     kMagic[4] = {'V', 'X', 'C', 'K'};
constexpr uint32_t kVersion  = 1;

// ── Byte writer ────────────────────────────────────────────────────────────
void putU8 (std::vector<uint8_t>& b, uint8_t v)  { b.push_back(v); }
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
void putI32(std::vector<uint8_t>& b, int32_t v) {
    uint32_t u; std::memcpy(&u, &v, 4); putU32(b, u);
}
void putF32(std::vector<uint8_t>& b, float v) {
    uint32_t u; std::memcpy(&u, &v, 4); putU32(b, u);
}
void putF64(std::vector<uint8_t>& b, double v) {
    uint64_t u; std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xff));
}

// ── Byte reader (bounds-checked; ok stays true while reads succeed) ───────────
struct Reader {
    const uint8_t* p;
    size_t         remaining;
    bool           ok = true;

    bool take(void* dst, size_t n) {
        if (!ok || remaining < n) { ok = false; return false; }
        std::memcpy(dst, p, n);
        p += n; remaining -= n;
        return true;
    }
    uint8_t  u8()  { uint8_t v = 0;  take(&v, 1); return v; }
    uint32_t u32() { uint8_t b[4]{}; take(b, 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24); }
    int32_t  i32() { uint32_t u = u32(); int32_t v; std::memcpy(&v, &u, 4); return v; }
    float    f32() { uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); return v; }
    double   f64() { uint8_t b[8]{}; take(b, 8);
        uint64_t u = 0; for (int i = 0; i < 8; ++i) u |= uint64_t(b[i]) << (8 * i);
        double v; std::memcpy(&v, &u, 8); return v; }
};

bool sameMaterial(const MaterialProperties& a, const MaterialProperties& b) {
    return a.density == b.density &&
           a.structural_strength == b.structural_strength &&
           a.thermal_conductivity == b.thermal_conductivity &&
           a.porosity == b.porosity &&
           a.hardness == b.hardness &&
           a.palette_index == b.palette_index;
}

}  // namespace

std::vector<uint8_t> encodeChunkFile(const Chunk& chunk, const WorldIdentity& id) {
    const int      n     = chunk.size();
    const size_t   count = static_cast<size_t>(n) * n * n;
    const Voxel*   vox   = chunk.data();

    // Build a palette of distinct materials and the per-voxel index stream.
    std::vector<MaterialProperties> palette;
    std::vector<uint32_t>           indices(count);
    for (size_t i = 0; i < count; ++i) {
        const MaterialProperties& m = vox[i].material;
        uint32_t idx = 0;
        bool found = false;
        for (; idx < palette.size(); ++idx)
            if (sameMaterial(palette[idx], m)) { found = true; break; }
        if (!found) { idx = static_cast<uint32_t>(palette.size()); palette.push_back(m); }
        indices[i] = idx;
    }

    std::vector<uint8_t> b;
    b.insert(b.end(), kMagic, kMagic + 4);
    putU32(b, kVersion);
    putF64(b, id.voxel_size_m);
    putU32(b, static_cast<uint32_t>(id.chunk_size_voxels));
    const ChunkCoord c = chunk.coord();
    putI32(b, c.x); putI32(b, c.y); putI32(b, c.z);

    putU32(b, static_cast<uint32_t>(palette.size()));
    for (const MaterialProperties& m : palette) {
        putF32(b, m.density);
        putF32(b, m.structural_strength);
        putF32(b, m.thermal_conductivity);
        putF32(b, m.porosity);
        putF32(b, m.hardness);
        putU8 (b, m.palette_index);
    }

    // Run-length encode the index stream (x-fastest flat order).
    std::vector<std::pair<uint32_t, uint32_t>> runs;  // (length, palette_idx)
    for (size_t i = 0; i < count; ) {
        uint32_t idx = indices[i];
        size_t   j   = i + 1;
        while (j < count && indices[j] == idx) ++j;
        runs.emplace_back(static_cast<uint32_t>(j - i), idx);
        i = j;
    }
    putU32(b, static_cast<uint32_t>(runs.size()));
    for (const auto& r : runs) { putU32(b, r.first); putU32(b, r.second); }

    return b;
}

std::unique_ptr<Chunk> decodeChunkFile(const uint8_t* data, size_t size,
                                       const WorldIdentity& id) {
    Reader r{data, size};

    char magic[4];
    if (!r.take(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return nullptr;
    if (r.u32() != kVersion) return nullptr;

    const double voxelSize  = r.f64();
    const int    chunkSize  = static_cast<int>(r.u32());
    if (!r.ok) return nullptr;
    if (voxelSize != id.voxel_size_m || chunkSize != id.chunk_size_voxels) return nullptr;
    if (chunkSize <= 0) return nullptr;

    ChunkCoord coord{r.i32(), r.i32(), r.i32()};
    if (!r.ok) return nullptr;

    const uint32_t paletteCount = r.u32();
    if (!r.ok || paletteCount == 0) return nullptr;
    std::vector<MaterialProperties> palette(paletteCount);
    for (uint32_t i = 0; i < paletteCount; ++i) {
        MaterialProperties& m = palette[i];
        m.density              = r.f32();
        m.structural_strength  = r.f32();
        m.thermal_conductivity = r.f32();
        m.porosity             = r.f32();
        m.hardness             = r.f32();
        m.palette_index        = r.u8();
    }
    if (!r.ok) return nullptr;

    const size_t   expected = static_cast<size_t>(chunkSize) * chunkSize * chunkSize;
    const uint32_t runCount = r.u32();
    if (!r.ok) return nullptr;

    WorldCoord origin = chunkmath::chunkOrigin(coord, voxelSize, chunkSize);
    auto chunk = std::make_unique<Chunk>(coord, chunkSize, origin);
    Voxel* out = chunk->data();

    size_t filled = 0;
    for (uint32_t i = 0; i < runCount; ++i) {
        const uint32_t length = r.u32();
        const uint32_t idx    = r.u32();
        if (!r.ok || idx >= paletteCount || length == 0) return nullptr;
        if (filled + length > expected) return nullptr;  // overrun
        Voxel v;
        v.material = palette[idx];
        for (uint32_t k = 0; k < length; ++k) out[filled++] = v;
    }
    if (filled != expected) return nullptr;  // grid not fully covered

    return chunk;
}

// ── WorldSave ────────────────────────────────────────────────────────────────

WorldSave::WorldSave(std::string dir, WorldIdentity id)
    : dir_(std::move(dir)), id_(id) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec)
        std::cerr << "[WorldSave] could not create save dir '" << dir_
                  << "': " << ec.message() << "\n";
}

std::string WorldSave::filePath(ChunkCoord coord) const {
    return (std::filesystem::path(dir_) /
            ("c_" + std::to_string(coord.x) + "_" + std::to_string(coord.y) + "_" +
             std::to_string(coord.z) + ".vxc")).string();
}

bool WorldSave::hasChunk(ChunkCoord coord) const {
    std::error_code ec;
    return std::filesystem::exists(filePath(coord), ec);
}

bool WorldSave::saveChunk(const Chunk& chunk) const {
    const std::vector<uint8_t> bytes = encodeChunkFile(chunk, id_);
    std::ofstream out(filePath(chunk.coord()), std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[WorldSave] could not open for write: " << filePath(chunk.coord()) << "\n";
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

int WorldSave::saveDirtyChunks(World& world) const {
    int saved = 0;
    for (const ChunkCoord c : world.dirtyChunkCoords()) {
        const Chunk* chunk = world.getChunk(c);
        if (chunk && saveChunk(*chunk)) {
            world.clearChunkDirty(c);
            ++saved;
        }
    }
    return saved;
}

std::unique_ptr<Chunk> WorldSave::tryLoadChunk(ChunkCoord coord) const {
    const std::string path = filePath(coord);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return nullptr;
    const std::streamsize size = in.tellg();
    if (size <= 0) return nullptr;
    in.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;
    return decodeChunkFile(bytes.data(), bytes.size(), id_);
}

}  // namespace persistence

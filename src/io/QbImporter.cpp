#include "QbImporter.h"

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

// ── Binary reader helpers ────────────────────────────────────────────────────

namespace {

struct Reader {
    const uint8_t* data;
    size_t         size;
    size_t         pos = 0;

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
};

// RLE sentinel values used by the Qubicle Binary compressed format.
constexpr uint32_t kCodeFlag      = 2;
constexpr uint32_t kNextSliceFlag = 6;

}  // anonymous namespace

// ── qb::parse ────────────────────────────────────────────────────────────────

namespace qb {

bool parse(const uint8_t* data, size_t size, QbFile& out) {
    Reader r{data, size};

    if (!r.readU32(out.version))              return false;
    if (!r.readU32(out.colorFormat))          return false;
    if (!r.readU32(out.zAxisOrientation))     return false;
    if (!r.readU32(out.compressed))           return false;
    if (!r.readU32(out.visibilityMaskEncoded)) return false;

    uint32_t numMatrices = 0;
    if (!r.readU32(numMatrices)) return false;

    out.matrices.reserve(numMatrices);

    for (uint32_t m = 0; m < numMatrices; ++m) {
        QbMatrix mat;

        // Name: length-prefixed string (uint8 length).
        uint8_t nameLen = 0;
        if (!r.readU8(nameLen)) return false;
        if (!r.canRead(nameLen)) return false;
        mat.name.assign(reinterpret_cast<const char*>(r.data + r.pos), nameLen);
        r.pos += nameLen;

        if (!r.readU32(mat.sizeX)) return false;
        if (!r.readU32(mat.sizeY)) return false;
        if (!r.readU32(mat.sizeZ)) return false;
        if (!r.readI32(mat.posX))  return false;
        if (!r.readI32(mat.posY))  return false;
        if (!r.readI32(mat.posZ))  return false;

        const size_t totalVoxels =
            static_cast<size_t>(mat.sizeX) * mat.sizeY * mat.sizeZ;
        mat.voxels.resize(totalVoxels, {0, 0, 0, 0});

        auto decodeColor = [&](uint32_t raw) -> RgbaColor {
            uint8_t b0 = static_cast<uint8_t>( raw        & 0xFF);
            uint8_t b1 = static_cast<uint8_t>((raw >>  8) & 0xFF);
            uint8_t b2 = static_cast<uint8_t>((raw >> 16) & 0xFF);
            uint8_t b3 = static_cast<uint8_t>((raw >> 24) & 0xFF);

            RgbaColor c;
            if (out.colorFormat == 1) {
                // BGRA byte order: b0=B, b1=G, b2=R, b3=A
                c = {b2, b1, b0, b3};
            } else {
                // RGBA byte order: b0=R, b1=G, b2=B, b3=A
                c = {b0, b1, b2, b3};
            }

            if (out.visibilityMaskEncoded && c.a != 0)
                c.a = 255;

            return c;
        };

        if (out.compressed == 0) {
            // Uncompressed: sizeX * sizeY * sizeZ uint32 entries in z,y,x order.
            for (uint32_t z = 0; z < mat.sizeZ; ++z) {
                for (uint32_t y = 0; y < mat.sizeY; ++y) {
                    for (uint32_t x = 0; x < mat.sizeX; ++x) {
                        uint32_t raw;
                        if (!r.readU32(raw)) return false;
                        const size_t idx = static_cast<size_t>(z) *
                            (mat.sizeY * mat.sizeX) +
                            static_cast<size_t>(y) * mat.sizeX + x;
                        mat.voxels[idx] = decodeColor(raw);
                    }
                }
            }
        } else {
            // RLE compressed: per-slice (z) run-length encoding.
            for (uint32_t z = 0; z < mat.sizeZ; ++z) {
                size_t idx = 0;
                while (true) {
                    uint32_t raw;
                    if (!r.readU32(raw)) return false;

                    if (raw == kNextSliceFlag) break;

                    if (raw == kCodeFlag) {
                        uint32_t count;
                        if (!r.readU32(count)) return false;
                        uint32_t colorRaw;
                        if (!r.readU32(colorRaw)) return false;
                        RgbaColor c = decodeColor(colorRaw);
                        for (uint32_t i = 0; i < count && idx < static_cast<size_t>(mat.sizeX) * mat.sizeY; ++i, ++idx) {
                            const uint32_t x = static_cast<uint32_t>(idx % mat.sizeX);
                            const uint32_t y = static_cast<uint32_t>(idx / mat.sizeX);
                            const size_t vi = static_cast<size_t>(z) *
                                (mat.sizeY * mat.sizeX) +
                                static_cast<size_t>(y) * mat.sizeX + x;
                            mat.voxels[vi] = c;
                        }
                    } else {
                        RgbaColor c = decodeColor(raw);
                        if (idx < static_cast<size_t>(mat.sizeX) * mat.sizeY) {
                            const uint32_t x = static_cast<uint32_t>(idx % mat.sizeX);
                            const uint32_t y = static_cast<uint32_t>(idx / mat.sizeX);
                            const size_t vi = static_cast<size_t>(z) *
                                (mat.sizeY * mat.sizeX) +
                                static_cast<size_t>(y) * mat.sizeX + x;
                            mat.voxels[vi] = c;
                        }
                        ++idx;
                    }
                }
            }
        }

        out.matrices.push_back(std::move(mat));
    }

    return true;
}

}  // namespace qb

// ── QbImporter::load ─────────────────────────────────────────────────────────

bool QbImporter::load(const std::string& path, Layer& layer,
                      const WorldCoord& anchor, const PluginManager& plugins) {
    // Read file
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "QbImporter: cannot open '%s'\n", path.c_str());
        return false;
    }
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        std::fprintf(stderr, "QbImporter: read error '%s'\n", path.c_str());
        return false;
    }

    // Parse
    qb::QbFile file;
    if (!qb::parse(buf.data(), buf.size(), file)) {
        std::fprintf(stderr, "QbImporter: parse error '%s'\n", path.c_str());
        return false;
    }

    // Build a mapping from unique RGBA colors to palette indices (1..255).
    // The .qb format stores raw colors, not palette indices.
    struct ColorKey {
        uint8_t r, g, b, a;
        bool operator==(const ColorKey& o) const {
            return r == o.r && g == o.g && b == o.b && a == o.a;
        }
    };
    struct ColorHash {
        size_t operator()(const ColorKey& c) const {
            return static_cast<size_t>(c.r)
                 | (static_cast<size_t>(c.g) << 8)
                 | (static_cast<size_t>(c.b) << 16)
                 | (static_cast<size_t>(c.a) << 24);
        }
    };
    std::unordered_map<ColorKey, uint8_t, ColorHash> colorToIndex;
    uint8_t nextIndex = 1;

    for (const auto& mat : file.matrices) {
        for (const auto& vc : mat.voxels) {
            if (vc.a == 0) continue;
            ColorKey key{vc.r, vc.g, vc.b, vc.a};
            if (colorToIndex.find(key) == colorToIndex.end()) {
                if (nextIndex == 0) {
                    std::fprintf(stderr,
                        "QbImporter: more than 255 unique colors; excess dropped\n");
                    break;
                }
                colorToIndex[key] = nextIndex++;
            }
        }
        if (nextIndex == 0) break;
    }

    // Install the mapped colors into the engine's visual palette.
    for (const auto& [key, idx] : colorToIndex) {
        const uint32_t abgr = static_cast<uint32_t>(key.r)
                            | (static_cast<uint32_t>(key.g) << 8)
                            | (static_cast<uint32_t>(key.b) << 16)
                            | (static_cast<uint32_t>(key.a) << 24);
        palette::setColor(idx, abgr);
    }

    // Pre-create any chunks that will be needed so setVoxel can find them.
    const double vsm = layer.voxelSizeM();
    const int    csv = layer.chunkSizeVoxels();

    std::unordered_set<ChunkCoord, ChunkCoordHash> needed;
    for (const auto& mat : file.matrices) {
        const int32_t halfX = static_cast<int32_t>(mat.sizeX) / 2;
        const int32_t halfY = static_cast<int32_t>(mat.sizeY) / 2;
        const int32_t halfZ = static_cast<int32_t>(mat.sizeZ) / 2;

        for (uint32_t z = 0; z < mat.sizeZ; ++z) {
            for (uint32_t y = 0; y < mat.sizeY; ++y) {
                for (uint32_t x = 0; x < mat.sizeX; ++x) {
                    const size_t vi = static_cast<size_t>(z) *
                        (mat.sizeY * mat.sizeX) +
                        static_cast<size_t>(y) * mat.sizeX + x;
                    const qb::RgbaColor& vc = mat.voxels[vi];
                    if (vc.a == 0) continue;

                    const double wx = anchor.value.x +
                        static_cast<double>(mat.posX + static_cast<int32_t>(x) - halfX) * vsm;
                    const double wy = anchor.value.y +
                        static_cast<double>(mat.posY + static_cast<int32_t>(y) - halfY) * vsm;
                    const double wz = anchor.value.z +
                        static_cast<double>(mat.posZ + static_cast<int32_t>(z) - halfZ) * vsm;
                    needed.insert(chunkmath::worldToChunk(WorldCoord(wx, wy, wz), vsm, csv));
                }
            }
        }
    }

    for (const ChunkCoord& cc : needed)
        layer.loadChunk(cc, nullptr);

    // Write voxels
    for (const auto& mat : file.matrices) {
        const int32_t halfX = static_cast<int32_t>(mat.sizeX) / 2;
        const int32_t halfY = static_cast<int32_t>(mat.sizeY) / 2;
        const int32_t halfZ = static_cast<int32_t>(mat.sizeZ) / 2;

        for (uint32_t z = 0; z < mat.sizeZ; ++z) {
            for (uint32_t y = 0; y < mat.sizeY; ++y) {
                for (uint32_t x = 0; x < mat.sizeX; ++x) {
                    const size_t vi = static_cast<size_t>(z) *
                        (mat.sizeY * mat.sizeX) +
                        static_cast<size_t>(y) * mat.sizeX + x;
                    const qb::RgbaColor& vc = mat.voxels[vi];
                    if (vc.a == 0) continue;

                    ColorKey key{vc.r, vc.g, vc.b, vc.a};
                    auto it = colorToIndex.find(key);
                    if (it == colorToIndex.end()) continue;

                    const double wx = anchor.value.x +
                        static_cast<double>(mat.posX + static_cast<int32_t>(x) - halfX) * vsm;
                    const double wy = anchor.value.y +
                        static_cast<double>(mat.posY + static_cast<int32_t>(y) - halfY) * vsm;
                    const double wz = anchor.value.z +
                        static_cast<double>(mat.posZ + static_cast<int32_t>(z) - halfZ) * vsm;

                    Voxel v;
                    v.material = plugins.materialForPalette(it->second);
                    layer.setVoxel(WorldCoord(wx, wy, wz), v);
                }
            }
        }
    }

    return true;
}

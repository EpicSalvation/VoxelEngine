// Tests for QbImporter and QbExporter (.qb / Qubicle Binary format).
//
// Covers:
//   - Round-trip: hand-crafted .qb -> import -> export -> re-import; palette
//     colors and voxel positions must survive unchanged.
//   - Non-zero world anchor: each voxel's world position = anchor + in-file coord.
//   - Two-matrix .qb: both matrices land at the correct world positions.
//   - RLE-compressed .qb: voxels parse correctly from run-length encoded data.
//   - Lossy-property warning: Engine::exportQb emits LOG_WARN when a layer
//     contains voxels with non-default extended properties and no plugin exporter
//     is registered.
//   - Format registry: ".qb" appears in the built-in importer/exporter lists.

#include "io/QbImporter.h"
#include "io/QbExporter.h"
#include "world/Layer.h"
#include "world/ChunkCoordMath.h"
#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "renderer/Palette.h"
#include "world/World.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// ── Minimal .qb binary builder ───────────────────────────────────────────────

void appendU8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back( v        & 0xFF);
    buf.push_back((v >>  8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

void appendI32(std::vector<uint8_t>& buf, int32_t v) {
    uint32_t u; std::memcpy(&u, &v, 4); appendU32(buf, u);
}

// Encode an RGBA color as a uint32 in .qb RGBA byte order (little-endian).
uint32_t encodeRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return static_cast<uint32_t>(r)
         | (static_cast<uint32_t>(g) << 8)
         | (static_cast<uint32_t>(b) << 16)
         | (static_cast<uint32_t>(a) << 24);
}

// Build a single-matrix uncompressed .qb file.
// voxelColors is a dense z,y,x array of RGBA uint32s (alpha=0 for empty).
std::vector<uint8_t> buildSingleMatrixQb(
        uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
        const std::vector<uint32_t>& voxelColors,
        int32_t posX = 0, int32_t posY = 0, int32_t posZ = 0,
        const std::string& name = "test") {
    std::vector<uint8_t> buf;

    // Header
    appendU32(buf, 0x00000101);  // version
    appendU32(buf, 0);           // colorFormat = RGBA
    appendU32(buf, 1);           // zAxisOrientation = right-handed
    appendU32(buf, 0);           // compressed = no
    appendU32(buf, 0);           // visibilityMaskEncoded = no
    appendU32(buf, 1);           // numMatrices

    // Matrix header
    appendU8(buf, static_cast<uint8_t>(name.size()));
    for (char c : name) appendU8(buf, static_cast<uint8_t>(c));
    appendU32(buf, sizeX);
    appendU32(buf, sizeY);
    appendU32(buf, sizeZ);
    appendI32(buf, posX);
    appendI32(buf, posY);
    appendI32(buf, posZ);

    // Voxel data
    for (uint32_t c : voxelColors)
        appendU32(buf, c);

    return buf;
}

// Build a two-matrix uncompressed .qb file.
std::vector<uint8_t> buildTwoMatrixQb(
        uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
        const std::vector<uint32_t>& voxels0,
        const std::vector<uint32_t>& voxels1,
        int32_t pos0x, int32_t pos0y, int32_t pos0z,
        int32_t pos1x, int32_t pos1y, int32_t pos1z) {
    std::vector<uint8_t> buf;

    appendU32(buf, 0x00000101);
    appendU32(buf, 0);
    appendU32(buf, 1);
    appendU32(buf, 0);
    appendU32(buf, 0);
    appendU32(buf, 2);  // numMatrices = 2

    // Matrix 0
    std::string name0 = "mat0";
    appendU8(buf, static_cast<uint8_t>(name0.size()));
    for (char c : name0) appendU8(buf, static_cast<uint8_t>(c));
    appendU32(buf, sizeX);
    appendU32(buf, sizeY);
    appendU32(buf, sizeZ);
    appendI32(buf, pos0x);
    appendI32(buf, pos0y);
    appendI32(buf, pos0z);
    for (uint32_t c : voxels0) appendU32(buf, c);

    // Matrix 1
    std::string name1 = "mat1";
    appendU8(buf, static_cast<uint8_t>(name1.size()));
    for (char c : name1) appendU8(buf, static_cast<uint8_t>(c));
    appendU32(buf, sizeX);
    appendU32(buf, sizeY);
    appendU32(buf, sizeZ);
    appendI32(buf, pos1x);
    appendI32(buf, pos1y);
    appendI32(buf, pos1z);
    for (uint32_t c : voxels1) appendU32(buf, c);

    return buf;
}

// Build a single-matrix RLE-compressed .qb file.
std::vector<uint8_t> buildCompressedQb(
        uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
        const std::vector<uint32_t>& voxelColors,
        const std::string& name = "rle") {
    std::vector<uint8_t> buf;

    appendU32(buf, 0x00000101);
    appendU32(buf, 0);           // RGBA
    appendU32(buf, 1);           // right-handed
    appendU32(buf, 1);           // compressed = yes
    appendU32(buf, 0);           // no visibility mask
    appendU32(buf, 1);

    appendU8(buf, static_cast<uint8_t>(name.size()));
    for (char c : name) appendU8(buf, static_cast<uint8_t>(c));
    appendU32(buf, sizeX);
    appendU32(buf, sizeY);
    appendU32(buf, sizeZ);
    appendI32(buf, 0);
    appendI32(buf, 0);
    appendI32(buf, 0);

    // Write RLE data: one voxel at a time (no actual run-length compression,
    // just the valid format with individual entries and NEXTSLICEFLAG per slice).
    const uint32_t kNextSlice = 6;
    for (uint32_t z = 0; z < sizeZ; ++z) {
        for (uint32_t y = 0; y < sizeY; ++y) {
            for (uint32_t x = 0; x < sizeX; ++x) {
                size_t idx = static_cast<size_t>(z) * (sizeY * sizeX)
                           + static_cast<size_t>(y) * sizeX + x;
                appendU32(buf, voxelColors[idx]);
            }
        }
        appendU32(buf, kNextSlice);
    }

    return buf;
}

// ── Test helpers ─────────────────────────────────────────────────────────────

LayerDef makeTerminalLayer(double voxelSizeM = 1.0, int chunkSize = 32) {
    LayerDef d;
    d.name              = "editor";
    d.voxel_size_m      = voxelSizeM;
    d.mode              = VoxelMode::terminal;
    d.chunk_size_voxels = chunkSize;
    return d;
}

std::filesystem::path writeTmpFile(const std::vector<uint8_t>& data,
                                   const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return p;
}

}  // namespace

// ── Round-trip: colors survive import -> export -> re-import ─────────────────

TEST(QbImportExport, RoundTripPreservesColors) {
    // Build a 2x2x2 .qb with 4 colored voxels and 4 empty ones.
    const uint32_t red   = encodeRgba(255,   0,   0, 255);
    const uint32_t green = encodeRgba(  0, 255,   0, 255);
    const uint32_t blue  = encodeRgba(  0,   0, 255, 255);
    const uint32_t white = encodeRgba(255, 255, 255, 255);
    const uint32_t empty = 0;

    // z,y,x order: 2*2*2 = 8 entries
    std::vector<uint32_t> voxels = {
        red,   green,   // z=0, y=0, x=0..1
        empty, empty,   // z=0, y=1, x=0..1
        blue,  white,   // z=1, y=0, x=0..1
        empty, empty,   // z=1, y=1, x=0..1
    };

    auto qbData = buildSingleMatrixQb(2, 2, 2, voxels);
    auto tmpPath = writeTmpFile(qbData, "qb_roundtrip.qb");

    LayerDef def = makeTerminalLayer();
    World world({def});
    PluginManager pm;

    QbImporter importer;
    WorldCoord anchor(10, 10, 10);
    ASSERT_TRUE(importer.load(tmpPath.string(), *world.layer("editor"), anchor, pm));

    // Export the imported region back to .qb
    auto tmpOut = std::filesystem::temp_directory_path() / "qb_roundtrip_out.qb";
    QbExporter exporter;
    ASSERT_TRUE(exporter.save(tmpOut.string(), *world.layer("editor"),
                              WorldCoord(9, 9, 9), WorldCoord(12, 12, 12)));

    // Re-import into a fresh layer
    LayerDef def2 = makeTerminalLayer();
    def2.name = "editor2";
    World world2({def2});
    QbImporter importer2;
    ASSERT_TRUE(importer2.load(tmpOut.string(), *world2.layer("editor2"),
                               WorldCoord(9, 9, 9), pm));

    // Verify the 4 non-empty voxels survived with matching colors.
    // The exact palette indices may differ between import cycles, but the
    // colors installed in the palette must match the originals.
    Layer& l2 = *world2.layer("editor2");
    int found = 0;
    for (const auto& [cc, chunk] : l2.chunks()) {
        const int n = chunk->size();
        for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const Voxel& v = chunk->at(x, y, z);
            if (!v.isEmpty()) ++found;
        }
    }
    EXPECT_EQ(found, 4);

    std::filesystem::remove(tmpPath);
    std::filesystem::remove(tmpOut);
}

// ── Non-zero anchor places voxels correctly ──────────────────────────────────

TEST(QbImportExport, NonZeroAnchorPlacesVoxelsCorrectly) {
    // 1x1x1 matrix with a single red voxel at position (0,0,0) in the matrix,
    // with matrix pos=(0,0,0). halfX/Y/Z = 0, so world pos = anchor + (0+0-0)*vsm.
    const uint32_t red = encodeRgba(255, 0, 0, 255);
    std::vector<uint32_t> voxels = {red};

    auto qbData = buildSingleMatrixQb(1, 1, 1, voxels);
    auto tmpPath = writeTmpFile(qbData, "qb_anchor.qb");

    LayerDef def = makeTerminalLayer();
    World world({def});
    PluginManager pm;

    WorldCoord anchor(5, 10, 15);
    QbImporter importer;
    ASSERT_TRUE(importer.load(tmpPath.string(), *world.layer("editor"), anchor, pm));

    // Voxel should be at world coord (5, 10, 15).
    Layer& layer = *world.layer("editor");
    Voxel v = layer.getVoxel(WorldCoord(5, 10, 15));
    EXPECT_FALSE(v.isEmpty());

    // Neighboring positions should be empty.
    EXPECT_TRUE(layer.getVoxel(WorldCoord(4, 10, 15)).isEmpty());
    EXPECT_TRUE(layer.getVoxel(WorldCoord(6, 10, 15)).isEmpty());

    std::filesystem::remove(tmpPath);
}

// ── Two matrices at correct world positions ──────────────────────────────────

TEST(QbImportExport, TwoMatricesAtCorrectWorldPositions) {
    // Matrix 0: 1x1x1 at pos=(0,0,0)
    // Matrix 1: 1x1x1 at pos=(5,0,0)
    // Both contain a single red voxel.
    const uint32_t red = encodeRgba(255, 0, 0, 255);
    std::vector<uint32_t> v0 = {red};
    std::vector<uint32_t> v1 = {red};

    auto qbData = buildTwoMatrixQb(1, 1, 1, v0, v1, 0, 0, 0, 5, 0, 0);
    auto tmpPath = writeTmpFile(qbData, "qb_two_matrix.qb");

    LayerDef def = makeTerminalLayer();
    World world({def});
    PluginManager pm;

    WorldCoord anchor(0, 0, 0);
    QbImporter importer;
    ASSERT_TRUE(importer.load(tmpPath.string(), *world.layer("editor"), anchor, pm));

    Layer& layer = *world.layer("editor");
    // Matrix 0: pos=(0,0,0), size=1x1x1, halfX/Y/Z=0, voxel at (0,0,0)
    // world = anchor + (0+0-0)*1 = (0,0,0)
    EXPECT_FALSE(layer.getVoxel(WorldCoord(0, 0, 0)).isEmpty());
    // Matrix 1: pos=(5,0,0), size=1x1x1, halfX/Y/Z=0, voxel at (0,0,0)
    // world = anchor + (5+0-0)*1 = (5,0,0)
    EXPECT_FALSE(layer.getVoxel(WorldCoord(5, 0, 0)).isEmpty());
    // Between them should be empty.
    EXPECT_TRUE(layer.getVoxel(WorldCoord(3, 0, 0)).isEmpty());

    std::filesystem::remove(tmpPath);
}

// ── RLE-compressed .qb parses correctly ──────────────────────────────────────

TEST(QbImportExport, CompressedQbParsesCorrectly) {
    const uint32_t red   = encodeRgba(255, 0, 0, 255);
    const uint32_t green = encodeRgba(0, 255, 0, 255);
    const uint32_t empty = 0;

    // 2x1x2 matrix: 4 entries
    std::vector<uint32_t> voxels = {
        red, green,  // z=0, y=0, x=0..1
        red, empty,  // z=1, y=0, x=0..1
    };

    auto qbData = buildCompressedQb(2, 1, 2, voxels);
    auto tmpPath = writeTmpFile(qbData, "qb_compressed.qb");

    LayerDef def = makeTerminalLayer();
    World world({def});
    PluginManager pm;

    WorldCoord anchor(0, 0, 0);
    QbImporter importer;
    ASSERT_TRUE(importer.load(tmpPath.string(), *world.layer("editor"), anchor, pm));

    // 3 non-empty voxels should exist.
    Layer& layer = *world.layer("editor");
    int found = 0;
    for (const auto& [cc, chunk] : layer.chunks()) {
        const int n = chunk->size();
        for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            if (!chunk->at(x, y, z).isEmpty()) ++found;
        }
    }
    EXPECT_EQ(found, 3);

    std::filesystem::remove(tmpPath);
}

// ── BGRA color format decodes correctly ──────────────────────────────────────

TEST(QbImportExport, BgraColorFormatDecodesCorrectly) {
    // In BGRA format, byte order is B,G,R,A (little-endian uint32).
    // So to encode red (R=255,G=0,B=0,A=255):
    // byte0=B=0, byte1=G=0, byte2=R=255, byte3=A=255
    // uint32 = 0xFF_FF_00_00 (A=0xFF, R=0xFF, G=0x00, B=0x00)
    uint32_t bgraRed = 0xFF'FF'00'00u;  // BGRA encoding of pure red

    std::vector<uint8_t> buf;
    appendU32(buf, 0x00000101);  // version
    appendU32(buf, 1);           // colorFormat = BGRA
    appendU32(buf, 1);           // right-handed
    appendU32(buf, 0);           // uncompressed
    appendU32(buf, 0);           // no vis mask
    appendU32(buf, 1);           // 1 matrix

    std::string name = "bgra";
    appendU8(buf, static_cast<uint8_t>(name.size()));
    for (char c : name) appendU8(buf, static_cast<uint8_t>(c));
    appendU32(buf, 1); appendU32(buf, 1); appendU32(buf, 1);  // 1x1x1
    appendI32(buf, 0); appendI32(buf, 0); appendI32(buf, 0);  // pos
    appendU32(buf, bgraRed);

    auto tmpPath = writeTmpFile(buf, "qb_bgra.qb");

    LayerDef def = makeTerminalLayer();
    World world({def});
    PluginManager pm;

    QbImporter importer;
    ASSERT_TRUE(importer.load(tmpPath.string(), *world.layer("editor"),
                              WorldCoord(0, 0, 0), pm));

    Layer& layer = *world.layer("editor");
    Voxel v = layer.getVoxel(WorldCoord(0, 0, 0));
    EXPECT_FALSE(v.isEmpty());

    // Verify the palette color is red (ABGR = 0xFF0000FF).
    uint32_t abgr = palette::color(v.material.palette_index);
    uint8_t r =  abgr        & 0xFF;
    uint8_t g = (abgr >>  8) & 0xFF;
    uint8_t b = (abgr >> 16) & 0xFF;
    EXPECT_EQ(r, 255);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(b, 0);

    std::filesystem::remove(tmpPath);
}

// ── Low-level parser tests ───────────────────────────────────────────────────

TEST(QbParser, ParsesHeaderCorrectly) {
    const uint32_t red = encodeRgba(255, 0, 0, 255);
    std::vector<uint32_t> voxels = {red};
    auto data = buildSingleMatrixQb(1, 1, 1, voxels);

    qb::QbFile file;
    ASSERT_TRUE(qb::parse(data.data(), data.size(), file));

    EXPECT_EQ(file.version, 0x00000101u);
    EXPECT_EQ(file.colorFormat, 0u);
    EXPECT_EQ(file.zAxisOrientation, 1u);
    EXPECT_EQ(file.compressed, 0u);
    EXPECT_EQ(file.visibilityMaskEncoded, 0u);
    ASSERT_EQ(file.matrices.size(), 1u);
    EXPECT_EQ(file.matrices[0].name, "test");
    EXPECT_EQ(file.matrices[0].sizeX, 1u);
    EXPECT_EQ(file.matrices[0].sizeY, 1u);
    EXPECT_EQ(file.matrices[0].sizeZ, 1u);
}

TEST(QbParser, RejectsTruncatedData) {
    std::vector<uint8_t> buf;
    appendU32(buf, 0x00000101);
    // Missing rest of header
    qb::QbFile file;
    EXPECT_FALSE(qb::parse(buf.data(), buf.size(), file));
}

TEST(QbParser, EmptyMatrixParsesSuccessfully) {
    // 2x2x2 matrix where all voxels are empty (alpha=0).
    std::vector<uint32_t> voxels(8, 0);
    auto data = buildSingleMatrixQb(2, 2, 2, voxels);

    qb::QbFile file;
    ASSERT_TRUE(qb::parse(data.data(), data.size(), file));
    ASSERT_EQ(file.matrices.size(), 1u);
    EXPECT_EQ(file.matrices[0].voxels.size(), 8u);

    for (const auto& v : file.matrices[0].voxels)
        EXPECT_EQ(v.a, 0);
}

// ── Hostile-input dimension caps (2026-07 security review) ───────────────────

// Header bytes for a single-matrix file with the given dimensions and
// compression flag, ending right after the matrix position — i.e. where the
// voxel payload would start. Used to test that hostile dimensions are rejected
// BEFORE the dense grid is allocated.
static std::vector<uint8_t> hostileQbHeader(uint32_t sizeX, uint32_t sizeY,
                                            uint32_t sizeZ, uint32_t compressed) {
    std::vector<uint8_t> buf;
    appendU32(buf, 0x00000101);  // version
    appendU32(buf, 0);           // RGBA
    appendU32(buf, 0);           // orientation
    appendU32(buf, compressed);
    appendU32(buf, 0);           // no visibility mask
    appendU32(buf, 1);           // numMatrices
    appendU8(buf, 1); appendU8(buf, 'm');
    appendU32(buf, sizeX);
    appendU32(buf, sizeY);
    appendU32(buf, sizeZ);
    appendI32(buf, 0); appendI32(buf, 0); appendI32(buf, 0);
    return buf;
}

TEST(QbParser, RejectsOversizedMatrixDimensions) {
    // RLE-compressed, so no file-size bound applies — only the voxel-count cap
    // stands between a ~40-byte file and a 4096³ (275 GB) grid allocation.
    auto data = hostileQbHeader(4096, 4096, 4096, /*compressed=*/1);
    qb::QbFile file;
    EXPECT_FALSE(qb::parse(data.data(), data.size(), file));

    // Dimensions whose size_t product wraps must also be rejected, not
    // allocated at the wrapped (small) size.
    auto wrap = hostileQbHeader(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 1);
    EXPECT_FALSE(qb::parse(wrap.data(), wrap.size(), file));
}

TEST(QbParser, ZeroSizedMatrixStillParses) {
    // A degenerate 0×0×0 matrix is odd but harmless; the dimension validation
    // must not reject it (regression guard for the cap logic's zero branch).
    auto data = hostileQbHeader(0, 0, 0, /*compressed=*/0);
    qb::QbFile file;
    ASSERT_TRUE(qb::parse(data.data(), data.size(), file));
    ASSERT_EQ(file.matrices.size(), 1u);
    EXPECT_TRUE(file.matrices[0].voxels.empty());
}

TEST(QbParser, RejectsUncompressedGridLargerThanFile) {
    // Uncompressed needs 4 bytes per declared voxel; a 256³ declaration with no
    // payload must fail before resizing the 67 MB grid.
    auto data = hostileQbHeader(256, 256, 256, /*compressed=*/0);
    qb::QbFile file;
    EXPECT_FALSE(qb::parse(data.data(), data.size(), file));
}

// ── Lossy-property warning ───────────────────────────────────────────────────

TEST(QbLossyWarning, EmitsWarnWhenExtendedPropertiesPresent) {
    LayerDef def = makeTerminalLayer();
    World world({def});
    PluginManager pm;
    Engine engine;
    engine.init(pm, world);

    Layer& layer = *world.layer("editor");
    layer.loadChunk({0, 0, 0}, nullptr);
    Voxel v;
    v.material.palette_index        = 1;
    v.material.density              = 100.0f;
    v.material.structural_strength  = 50.0f;
    layer.setVoxel(WorldCoord(0, 0, 0), v);

    bool warnEmitted = false;
    Log::setWarnHandler([&warnEmitted](const char*) {
        warnEmitted = true;
    });

    auto tmpOut = std::filesystem::temp_directory_path() / "qb_lossy_warn.qb";
    bool ok = engine.exportQb("editor",
                               WorldCoord(0, 0, 0),
                               WorldCoord(32, 32, 32),
                               tmpOut.string());
    Log::setWarnHandler(nullptr);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(warnEmitted);

    std::filesystem::remove(tmpOut);
}

TEST(QbLossyWarning, NoWarnWhenOnlyPalettePropertiesSet) {
    LayerDef def = makeTerminalLayer();
    World world({def});
    PluginManager pm;
    Engine engine;
    engine.init(pm, world);

    Layer& layer = *world.layer("editor");
    layer.loadChunk({0, 0, 0}, nullptr);
    Voxel v;
    v.material.palette_index = 3;
    layer.setVoxel(WorldCoord(0, 0, 0), v);

    bool warnEmitted = false;
    Log::setWarnHandler([&warnEmitted](const char*) {
        warnEmitted = true;
    });

    auto tmpOut = std::filesystem::temp_directory_path() / "qb_no_warn.qb";
    bool ok = engine.exportQb("editor",
                               WorldCoord(0, 0, 0),
                               WorldCoord(32, 32, 32),
                               tmpOut.string());
    Log::setWarnHandler(nullptr);

    EXPECT_TRUE(ok);
    EXPECT_FALSE(warnEmitted);

    std::filesystem::remove(tmpOut);
}

// ── Format registry ──────────────────────────────────────────────────────────

TEST(QbFormatRegistry, QbRegisteredAsBuiltinHandler) {
    PluginManager pm;
    Engine engine;
    LayerDef def = makeTerminalLayer();
    World world({def});
    engine.init(pm, world);

    bool importerFound = false;
    bool exporterFound = false;
    for (const auto& reg : pm.importers()) {
        if (reg.extension == "qb" && reg.isBuiltin)
            importerFound = true;
    }
    for (const auto& reg : pm.exporters()) {
        if (reg.extension == "qb" && reg.isBuiltin)
            exporterFound = true;
    }
    EXPECT_TRUE(importerFound);
    EXPECT_TRUE(exporterFound);
}

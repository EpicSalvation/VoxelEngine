// Tests for VoxImporter (M7 group 1) and VoxExporter (M7 group 2).
//
// Covers:
//   - Round-trip: hand-crafted .vox → import → export → re-import; palette_index
//     values must survive unchanged.
//   - Non-zero world anchor: each voxel's world position = anchor + in-file coord.
//   - Two-object .vox with nTRN offsets: both objects land at the correct
//     world positions relative to the anchor.
//   - Auto-chunking export: a 300×1×1 region produces exactly two objects with
//     nTRN offsets that split at the 256-voxel boundary.
//   - Lossy-property warning: Engine::exportVox emits LOG_WARN when a layer
//     contains voxels with non-default extended properties and no plugin exporter
//     is registered.

#include "io/VoxImporter.h"
#include "io/VoxExporter.h"
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

// ── Minimal .vox binary builder ───────────────────────────────────────────────

void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back( v        & 0xFF);
    buf.push_back((v >>  8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

void appendI32(std::vector<uint8_t>& buf, int32_t v) {
    uint32_t u; std::memcpy(&u, &v, 4); appendU32(buf, u);
}

void appendStr(std::vector<uint8_t>& buf, const std::string& s) {
    appendI32(buf, static_cast<int32_t>(s.size()));
    for (char c : s) buf.push_back(static_cast<uint8_t>(c));
}

void appendChunk(std::vector<uint8_t>& buf, const char* id,
                 const std::vector<uint8_t>& content,
                 const std::vector<uint8_t>& children = {}) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(id[i]));
    appendU32(buf, static_cast<uint32_t>(content.size()));
    appendU32(buf, static_cast<uint32_t>(children.size()));
    buf.insert(buf.end(), content.begin(), content.end());
    buf.insert(buf.end(), children.begin(), children.end());
}

// Build a minimal single-model .vox with a hand-crafted palette.
// voxels = {{x, y, z, colorIndex}, ...}, palette = {{r,g,b,a} for indices 1..255}.
std::vector<uint8_t> buildSingleModelVox(
        uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
        const std::vector<std::array<uint8_t, 4>>& voxels,
        const std::array<std::array<uint8_t, 4>, 255>& palette) {
    // SIZE content
    std::vector<uint8_t> sizeContent;
    appendU32(sizeContent, sizeX);
    appendU32(sizeContent, sizeY);
    appendU32(sizeContent, sizeZ);

    // XYZI content
    std::vector<uint8_t> xyziContent;
    appendU32(xyziContent, static_cast<uint32_t>(voxels.size()));
    for (const auto& v : voxels) {
        xyziContent.push_back(v[0]); xyziContent.push_back(v[1]);
        xyziContent.push_back(v[2]); xyziContent.push_back(v[3]);
    }

    // RGBA content: 255 entries + 1 dummy
    std::vector<uint8_t> rgbaContent;
    for (const auto& p : palette) {
        rgbaContent.push_back(p[0]); rgbaContent.push_back(p[1]);
        rgbaContent.push_back(p[2]); rgbaContent.push_back(p[3]);
    }
    // 256th entry (dummy)
    rgbaContent.push_back(0); rgbaContent.push_back(0);
    rgbaContent.push_back(0); rgbaContent.push_back(0);

    std::vector<uint8_t> children;
    appendChunk(children, "SIZE", sizeContent);
    appendChunk(children, "XYZI", xyziContent);
    appendChunk(children, "RGBA", rgbaContent);

    std::vector<uint8_t> file;
    file.push_back('V'); file.push_back('O');
    file.push_back('X'); file.push_back(' ');
    appendU32(file, 150);  // version
    appendChunk(file, "MAIN", {}, children);
    return file;
}

// Build a two-model .vox with nTRN scene graph.
// Model 0: at nTRN offset (off0x, off0y, off0z)
// Model 1: at nTRN offset (off1x, off1y, off1z)
std::vector<uint8_t> buildTwoModelVox(
        uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
        const std::vector<std::array<uint8_t, 4>>& voxels0,
        const std::vector<std::array<uint8_t, 4>>& voxels1,
        int32_t off0x, int32_t off0y, int32_t off0z,
        int32_t off1x, int32_t off1y, int32_t off1z) {
    auto makeSize = [&]() {
        std::vector<uint8_t> c;
        appendU32(c, sizeX); appendU32(c, sizeY); appendU32(c, sizeZ);
        return c;
    };
    auto makeXyzi = [](const std::vector<std::array<uint8_t, 4>>& vs) {
        std::vector<uint8_t> c;
        appendU32(c, static_cast<uint32_t>(vs.size()));
        for (const auto& v : vs) {
            c.push_back(v[0]); c.push_back(v[1]);
            c.push_back(v[2]); c.push_back(v[3]);
        }
        return c;
    };

    // nTRN content helper
    auto makeTrn = [](int32_t nodeId, int32_t childId,
                      int32_t tx, int32_t ty, int32_t tz) {
        std::vector<uint8_t> c;
        appendI32(c, nodeId);
        appendI32(c, 0);       // empty attr dict
        appendI32(c, childId);
        appendI32(c, -1);
        appendI32(c, -1);
        appendI32(c, 1);       // 1 frame
        // frame dict with _t
        appendI32(c, 1);
        appendStr(c, "_t");
        std::string t = std::to_string(tx) + " " + std::to_string(ty) + " " + std::to_string(tz);
        appendStr(c, t);
        return c;
    };
    auto makeGrp = [](int32_t nodeId, const std::vector<int32_t>& kids) {
        std::vector<uint8_t> c;
        appendI32(c, nodeId);
        appendI32(c, 0);
        appendI32(c, static_cast<int32_t>(kids.size()));
        for (int32_t k : kids) appendI32(c, k);
        return c;
    };
    auto makeShp = [](int32_t nodeId, int32_t modelId) {
        std::vector<uint8_t> c;
        appendI32(c, nodeId);
        appendI32(c, 0);
        appendI32(c, 1);
        appendI32(c, modelId);
        appendI32(c, 0);
        return c;
    };

    std::vector<uint8_t> children;
    appendChunk(children, "SIZE", makeSize());
    appendChunk(children, "XYZI", makeXyzi(voxels0));
    appendChunk(children, "SIZE", makeSize());
    appendChunk(children, "XYZI", makeXyzi(voxels1));
    // Minimal RGBA (all gray, we only care about positions)
    std::vector<uint8_t> rgba(1024, 128);
    appendChunk(children, "RGBA", rgba);
    // Scene graph: nTRN(0)→nGRP(1)→[nTRN(2)→nSHP(3), nTRN(4)→nSHP(5)]
    appendChunk(children, "nTRN", makeTrn(0, 1, 0, 0, 0));
    appendChunk(children, "nGRP", makeGrp(1, {2, 4}));
    appendChunk(children, "nTRN", makeTrn(2, 3, off0x, off0y, off0z));
    appendChunk(children, "nSHP", makeShp(3, 0));
    appendChunk(children, "nTRN", makeTrn(4, 5, off1x, off1y, off1z));
    appendChunk(children, "nSHP", makeShp(5, 1));

    std::vector<uint8_t> file;
    file.push_back('V'); file.push_back('O');
    file.push_back('X'); file.push_back(' ');
    appendU32(file, 200);
    appendChunk(file, "MAIN", {}, children);
    return file;
}

// ── Test helpers ──────────────────────────────────────────────────────────────

LayerDef makeTerminalLayer(double voxelSizeM = 1.0, int chunkSize = 32) {
    LayerDef d;
    d.name              = "editor";
    d.voxel_size_m      = voxelSizeM;
    d.mode              = VoxelMode::terminal;
    d.chunk_size_voxels = chunkSize;
    return d;
}

// Write bytes to a temp file, return the path.
std::filesystem::path writeTmpFile(const std::vector<uint8_t>& data,
                                   const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return p;
}

}  // namespace

// ── Import round-trip: palette_index values survive import → export → re-import
//
// The .vox center-origin convention means each model's local (0,0,0) maps to
// anchor - halfSize.  The exporter always emits a scene graph whose nTRN offsets
// re-centre the model at the export region's anchor on re-import, so the
// round-trip is transparent when the re-import uses the same anchor as the
// export minCorner.
TEST(VoxImportExport, RoundTripPreservesPaletteIndex) {
    // Use a 4×2×2 model (SIZE 4×2×2, halfX=2, halfY=1, halfZ=1) with 8 voxels
    // placed so that the two "half-centre" positions land exactly at whole voxels.
    // With anchor=(10,10,10):
    //   ve=(2,1,1,1) → wx = 10+(2-2)=10, wy=10+(1-1)=10, wz=10 → voxelCoord(10,10,10)
    //   ve=(3,1,1,2) → wx = 11, wy=10, wz=10 → voxelCoord(11,10,10)
    //   (remaining 6 voxels are also at half-centre positions)
    std::array<std::array<uint8_t, 4>, 255> pal{};
    for (auto& e : pal) e = {200, 200, 200, 255};
    pal[0] = {255,   0,   0, 255};  // palette index 1
    pal[1] = {  0, 255,   0, 255};  // palette index 2

    // 8 voxels arranged symmetrically around the model centre (2,1,1)
    std::vector<std::array<uint8_t, 4>> voxels = {
        {2,1,1,1}, {3,1,1,2},  // y=1,z=1 row
        {2,0,1,1}, {3,0,1,2},  // y=0,z=1 row
        {2,1,0,1}, {3,1,0,2},  // y=1,z=0 row
        {2,0,0,1}, {3,0,0,2},  // y=0,z=0 row
    };
    // SIZE = 4×2×2 so that the placed voxels span x=[2,3], y=[0,1], z=[0,1]
    const auto rawVox = buildSingleModelVox(4, 2, 2, voxels, pal);
    const auto tmpIn  = writeTmpFile(rawVox, "rt_in.vox");
    const auto tmpOut = std::filesystem::temp_directory_path() / "rt_out.vox";

    const WorldCoord anchor(10.0, 10.0, 10.0);
    LayerDef def = makeTerminalLayer(1.0, 32);
    Layer layer(def);
    PluginManager pm;
    VoxImporter importer;
    ASSERT_TRUE(importer.load(tmpIn.string(), layer, anchor, pm));

    // Determine the actual bounding box of imported voxels.
    // halfX=2, halfY=1, halfZ=1, offset=0:
    // ve=(2,1,1): wx=10+(2-2)=10, wy=10+(1-1)=10, wz=10
    // ve=(3,1,1): wx=11,           wy=10,           wz=10
    // ve=(2,0,0): wx=10, wy=10+(0-1)=9, wz=9
    // Bounding box: x=[9,12), y=[9,11), z=[9,11) (voxelCoords [9..11],[9..10],[9..10])
    const WorldCoord exportMin(9.0, 9.0, 9.0);
    const WorldCoord exportMax(12.0, 11.0, 11.0);

    VoxExporter exporter;
    ASSERT_TRUE(exporter.save(tmpOut.string(), layer, exportMin, exportMax));

    // Re-import with exportMin as anchor so nTRN offsets restore original positions.
    Layer layer2(def);
    ASSERT_TRUE(importer.load(tmpOut.string(), layer2, exportMin, pm));

    // Compare every non-empty voxel in the first layer against the second layer.
    int compared = 0;
    for (const auto& [cc, chunk] : layer.chunks()) {
        const int csv = def.chunk_size_voxels;
        for (int lz = 0; lz < csv; ++lz) {
            for (int ly = 0; ly < csv; ++ly) {
                for (int lx = 0; lx < csv; ++lx) {
                    const Voxel& v1 = chunk->at(lx, ly, lz);
                    if (v1.isEmpty()) continue;
                    const auto vc = chunkmath::chunkLocalToVoxel(cc, lx, ly, lz, csv);
                    const WorldCoord wc = chunkmath::voxelCenter(vc, def.voxel_size_m);
                    const Voxel v2 = layer2.getVoxel(wc);
                    EXPECT_FALSE(v2.isEmpty())
                        << "voxel missing in layer2 at " << vc.x << "," << vc.y << "," << vc.z;
                    EXPECT_EQ(v1.material.palette_index, v2.material.palette_index)
                        << "palette_index changed at " << vc.x << "," << vc.y << "," << vc.z;
                    ++compared;
                }
            }
        }
    }
    EXPECT_EQ(compared, 8) << "expected 8 non-empty voxels";

    std::filesystem::remove(tmpIn);
    std::filesystem::remove(tmpOut);
}

// ── Color fidelity: a .vox file's authored colors survive import → render
//    palette → export → re-parse, including a translucent (alpha < 255) entry.
//
// Import installs each used color index's RGBA into the engine's visual palette
// (so voxels render with their authored colors), and export writes that palette
// back out. This verifies both halves: palette::color after import, and the RGBA
// chunk of the exported file.
TEST(VoxImportExport, PreservesAuthoredColorsThroughRoundTrip) {
    // Distinct colors unlike the default palette; index 3 is translucent.
    const std::array<uint8_t, 4> c1 = {10, 20, 30, 255};
    const std::array<uint8_t, 4> c2 = {40, 50, 60, 255};
    const std::array<uint8_t, 4> c3 = {70, 80, 90, 128};  // translucent

    std::array<std::array<uint8_t, 4>, 255> pal{};
    for (auto& e : pal) e = {200, 200, 200, 255};
    pal[0] = c1;  // palette index 1
    pal[1] = c2;  // palette index 2
    pal[2] = c3;  // palette index 3

    // SIZE 3×1×1 (halfX=1, halfY=halfZ=0). With anchor=10 the three voxels land
    // at voxel coords x=9,10,11 (y=z=10), so the export region is easy to bound.
    std::vector<std::array<uint8_t, 4>> voxels = {{0,0,0,1}, {1,0,0,2}, {2,0,0,3}};
    const auto rawVox = buildSingleModelVox(3, 1, 1, voxels, pal);
    const auto tmpIn  = writeTmpFile(rawVox, "color_in.vox");
    const auto tmpOut = std::filesystem::temp_directory_path() / "color_out.vox";

    const WorldCoord anchor(10.0, 10.0, 10.0);
    LayerDef def = makeTerminalLayer(1.0, 32);
    Layer layer(def);
    PluginManager pm;
    VoxImporter importer;
    ASSERT_TRUE(importer.load(tmpIn.string(), layer, anchor, pm));

    // Pack an {r,g,b,a} test color into the engine's ABGR (0xAABBGGRR) word.
    auto abgr = [](const std::array<uint8_t, 4>& c) {
        return static_cast<uint32_t>(c[0])
             | (static_cast<uint32_t>(c[1]) << 8)
             | (static_cast<uint32_t>(c[2]) << 16)
             | (static_cast<uint32_t>(c[3]) << 24);
    };

    // Half 1: import installed the authored colors into the render palette.
    EXPECT_EQ(palette::color(1), abgr(c1));
    EXPECT_EQ(palette::color(2), abgr(c2));
    EXPECT_EQ(palette::color(3), abgr(c3));
    EXPECT_TRUE(palette::isTranslucent(3)) << "alpha 128 entry should be translucent";

    // Export the region covering the three voxels, then re-parse the raw bytes.
    const WorldCoord exportMin(9.0, 10.0, 10.0);
    const WorldCoord exportMax(12.0, 11.0, 11.0);
    VoxExporter exporter;
    ASSERT_TRUE(exporter.save(tmpOut.string(), layer, exportMin, exportMax));

    std::ifstream f(tmpOut, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> outBytes(static_cast<size_t>(sz));
    ASSERT_TRUE(f.read(reinterpret_cast<char*>(outBytes.data()), sz).good());
    f.close();

    vox::VoxFile parsed;
    ASSERT_TRUE(vox::parse(outBytes.data(), outBytes.size(), parsed));

    // Half 2: the exported RGBA chunk carries the same colors, alpha included.
    auto expectColor = [&](int idx, const std::array<uint8_t, 4>& c) {
        EXPECT_EQ(parsed.palette[idx].r, c[0]) << "R at index " << idx;
        EXPECT_EQ(parsed.palette[idx].g, c[1]) << "G at index " << idx;
        EXPECT_EQ(parsed.palette[idx].b, c[2]) << "B at index " << idx;
        EXPECT_EQ(parsed.palette[idx].a, c[3]) << "A at index " << idx;
    };
    expectColor(1, c1);
    expectColor(2, c2);
    expectColor(3, c3);

    std::filesystem::remove(tmpIn);
    std::filesystem::remove(tmpOut);
}

// ── Non-zero world anchor: voxel world position = anchor + in-file coordinate

TEST(VoxImportExport, NonZeroAnchorPlacesVoxelsCorrectly) {
    std::array<std::array<uint8_t, 4>, 255> pal{};
    for (auto& e : pal) e = {100, 100, 100, 255};
    // One voxel at in-file position (3, 0, 0) with palette index 5.
    std::vector<std::array<uint8_t, 4>> voxels = {{3, 0, 0, 5}};
    const auto rawVox = buildSingleModelVox(4, 1, 1, voxels, pal);
    const auto tmpIn  = writeTmpFile(rawVox, "anchor_test.vox");

    LayerDef def = makeTerminalLayer(1.0, 32);
    Layer layer(def);
    PluginManager pm;
    VoxImporter importer;

    // Import with anchor at (10, 0, 0).
    const WorldCoord anchor(10.0, 0.0, 0.0);
    ASSERT_TRUE(importer.load(tmpIn.string(), layer, anchor, pm));

    // Model size = 4; halfX = 2. Expected world x = anchor.x + (offsetX + 3 - 2) * 1
    // = 10 + (0 + 3 - 2) = 11. So the voxel is at world x ∈ [11, 12), center at 11.5.
    const WorldCoord expected(11.5, 0.5, 0.5);
    const Voxel v = layer.getVoxel(expected);
    EXPECT_FALSE(v.isEmpty()) << "voxel not found at expected world position";
    EXPECT_EQ(v.material.palette_index, 5);

    std::filesystem::remove(tmpIn);
}

// ── Two-object .vox with nTRN offsets: both objects at correct world positions

TEST(VoxImportExport, TwoObjectsAtCorrectWorldPositions) {
    // Two models each 2×1×1 voxels.
    // Model 0: in-file x=[0,1], nTRN offset (10, 0, 0) → world x =[10-1, 10+0]=[9,10]
    //   halfX=1, so world x for local=0: anchor + (10 + 0 - 1)*1 = 9
    //                           local=1: anchor + (10 + 1 - 1)*1 = 10
    // Model 1: in-file x=[0,1], nTRN offset (20, 0, 0) → world x = [19, 20]
    std::vector<std::array<uint8_t, 4>> vox0 = {{0,0,0,1}, {1,0,0,1}};
    std::vector<std::array<uint8_t, 4>> vox1 = {{0,0,0,2}, {1,0,0,2}};
    const auto rawVox = buildTwoModelVox(2, 1, 1, vox0, vox1,
                                         10, 0, 0,
                                         20, 0, 0);
    const auto tmpIn = writeTmpFile(rawVox, "two_obj.vox");

    LayerDef def = makeTerminalLayer(1.0, 32);
    Layer layer(def);
    PluginManager pm;
    VoxImporter importer;
    ASSERT_TRUE(importer.load(tmpIn.string(), layer, WorldCoord(0, 0, 0), pm));

    // Model 0, local x=0 → world x = 0 + (10 + 0 - 1)*1 = 9
    const Voxel v0a = layer.getVoxel(WorldCoord(9.5, 0.5, 0.5));
    EXPECT_FALSE(v0a.isEmpty());
    EXPECT_EQ(v0a.material.palette_index, 1);

    // Model 0, local x=1 → world x = 10
    const Voxel v0b = layer.getVoxel(WorldCoord(10.5, 0.5, 0.5));
    EXPECT_FALSE(v0b.isEmpty());
    EXPECT_EQ(v0b.material.palette_index, 1);

    // Model 1, local x=0 → world x = 0 + (20 + 0 - 1)*1 = 19
    const Voxel v1a = layer.getVoxel(WorldCoord(19.5, 0.5, 0.5));
    EXPECT_FALSE(v1a.isEmpty());
    EXPECT_EQ(v1a.material.palette_index, 2);

    // Model 1, local x=1 → world x = 20
    const Voxel v1b = layer.getVoxel(WorldCoord(20.5, 0.5, 0.5));
    EXPECT_FALSE(v1b.isEmpty());
    EXPECT_EQ(v1b.material.palette_index, 2);

    std::filesystem::remove(tmpIn);
}

// ── Export auto-chunking: 300×1×1 region → two objects split at x=256

TEST(VoxImportExport, AutoChunkingSplitsAt256) {
    // Build a 300-voxel long layer: all voxels have palette_index = 3.
    LayerDef def = makeTerminalLayer(1.0, 32);
    Layer layer(def);

    // Fill all 300 x-positions.
    for (int x = 0; x < 300; ++x) {
        // Make sure the owning chunk is resident.
        const WorldCoord wc(x + 0.5, 0.5, 0.5);
        const auto cc = chunkmath::worldToChunk(wc, 1.0, 32);
        layer.loadChunk(cc, nullptr);
        Voxel v;
        v.material.density       = 1.0f;
        v.material.palette_index = 3;
        layer.setVoxel(wc, v);
    }

    const auto tmpOut = std::filesystem::temp_directory_path() / "autochunk.vox";
    VoxExporter exporter;
    ASSERT_TRUE(exporter.save(tmpOut.string(), layer,
                              WorldCoord(0, 0, 0), WorldCoord(300, 1, 1)));

    // Parse the output and verify exactly two models with the correct nTRN offsets.
    // Scope ensures the ifstream is closed before std::filesystem::remove (required on Windows).
    {
        std::ifstream f(tmpOut, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(f.is_open());
        const std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        ASSERT_TRUE(f.read(reinterpret_cast<char*>(buf.data()), sz));

        vox::VoxFile parsed;
        ASSERT_TRUE(vox::parse(buf.data(), buf.size(), parsed));
        ASSERT_EQ(parsed.models.size(), 2u) << "expected exactly 2 models";

        // Model 0: size 256, model 1: size 44.
        const auto& m0 = parsed.models[0];
        const auto& m1 = parsed.models[1];
        EXPECT_EQ(m0.sizeX, 256u);
        EXPECT_EQ(m1.sizeX, 44u);

        // nTRN offsets: model 0 → tx = 0 + 256/2 = 128
        //               model 1 → tx = 256 + 44/2 = 278
        EXPECT_EQ(m0.offsetX, 128);
        EXPECT_EQ(m1.offsetX, 278);
        EXPECT_EQ(m0.offsetY, 0);
        EXPECT_EQ(m1.offsetY, 0);
    }

    // Re-import and verify voxels 0 and 255 are in model 0, voxels 256..299 in model 1.
    LayerDef def2 = makeTerminalLayer(1.0, 32);
    Layer layer2(def2);
    PluginManager pm;
    VoxImporter importer;
    ASSERT_TRUE(importer.load(tmpOut.string(), layer2,
                              WorldCoord(0, 0, 0), pm));

    // Spot-check boundary voxels survive the round-trip.
    for (int x : {0, 127, 255, 256, 299}) {
        const WorldCoord wc(x + 0.5, 0.5, 0.5);
        const Voxel v = layer2.getVoxel(wc);
        EXPECT_FALSE(v.isEmpty()) << "voxel at x=" << x << " missing after round-trip";
        EXPECT_EQ(v.material.palette_index, 3)
            << "palette_index wrong at x=" << x;
    }

    std::filesystem::remove(tmpOut);
}

// ── Lossy-property warning ─────────────────────────────────────────────────────
//
// Engine::exportVox must emit a LOG_WARN when:
//   - No plugin exporter is registered for ".vox" (only the built-in fallback).
//   - At least one voxel in the export region carries non-default extended
//     properties (density, structural_strength, thermal_conductivity, porosity,
//     or hardness != 0.0f) that the .vox format cannot represent.

TEST(VoxLossyWarning, EmitsWarnWhenExtendedPropertiesPresent) {
    LayerDef def = makeTerminalLayer(1.0, 32);
    World world(def);

    Layer* layer = world.primaryLayer();
    ASSERT_NE(layer, nullptr);
    layer->loadChunk({0, 0, 0}, nullptr);
    Voxel v;
    v.material.palette_index       = 1;
    v.material.density             = 2500.0f;  // non-default: .vox cannot store this
    v.material.structural_strength = 1.0f;
    layer->setVoxel(WorldCoord(0.5, 0.5, 0.5), v);

    PluginManager pm;
    Engine engine;
    engine.init(pm, world);

    std::vector<std::string> warnings;
    Log::setWarnHandler([&warnings](const char* msg) {
        warnings.emplace_back(msg);
    });

    auto tmpOut = std::filesystem::temp_directory_path() / "lossy_warn_test.vox";
    bool ok = engine.exportVox("editor",
                               WorldCoord(0, 0, 0),
                               WorldCoord(32, 32, 32),
                               tmpOut.string());
    EXPECT_TRUE(ok);
    Log::setWarnHandler(nullptr);

    bool found = false;
    for (const auto& w : warnings)
        if (w.find("extended voxel properties dropped") != std::string::npos)
            found = true;
    EXPECT_TRUE(found) << "Expected lossy-property warning was not emitted.";

    std::filesystem::remove(tmpOut);
}

TEST(VoxLossyWarning, NoWarnWhenOnlyPalettePropertiesSet) {
    LayerDef def = makeTerminalLayer(1.0, 32);
    World world(def);

    Layer* layer = world.primaryLayer();
    ASSERT_NE(layer, nullptr);
    layer->loadChunk({0, 0, 0}, nullptr);
    Voxel v;
    v.material.palette_index = 2;
    // All extended properties stay at their defaults (0.0f).
    layer->setVoxel(WorldCoord(0.5, 0.5, 0.5), v);

    PluginManager pm;
    Engine engine;
    engine.init(pm, world);

    std::vector<std::string> warnings;
    Log::setWarnHandler([&warnings](const char* msg) {
        warnings.emplace_back(msg);
    });

    auto tmpOut = std::filesystem::temp_directory_path() / "no_warn_test.vox";
    bool ok = engine.exportVox("editor",
                               WorldCoord(0, 0, 0),
                               WorldCoord(32, 32, 32),
                               tmpOut.string());
    EXPECT_TRUE(ok);
    Log::setWarnHandler(nullptr);

    for (const auto& w : warnings)
        EXPECT_EQ(w.find("extended voxel properties dropped"), std::string::npos)
            << "Unexpected lossy warning when only palette_index was set.";

    std::filesystem::remove(tmpOut);
}

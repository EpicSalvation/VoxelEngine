// Tests for the M15 T6 Blockbench importer plugin.
//
// Loads the real blockbench plugin and drives its registered .bbmodel ImporterFn
// directly (the engine's generic importer dispatch isn't bridged to the Layer API
// yet, so the importer is exercised the same way the dispatch eventually will be:
// file bytes + the plugin's ctx as user_data → a filled voxel grid plus textures
// and material-face bindings registered as a side effect).
//
// Asserts the full import contract: the sample model fills the grid with one
// imported material, its three referenced textures land in the atlas registry,
// the material's faces bind to the right tiles (top←up, bottom←down, side←a
// lateral face), and a malformed model FAILS rather than producing a silently-
// wrong block.

#include "core/PluginManager.h"
#include "renderer/MaterialFaces.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

#ifdef VOXEL_BLOCKBENCH_PLUGIN_PATH

using texture::AtlasTile;

namespace {

// A minimal hand-authored grass-block .bbmodel: three separate textures
// (grass_top, dirt, grass_side) and one full-block element whose up/down/north
// faces reference them. Texture sources are tiny base64 blobs — enough for the
// registry to record them (the headless test has no GPU atlas to decode into).
const char* kGrassBlock = R"BB({
  "resolution": {"width": 16, "height": 16},
  "textures": [
    {"name": "grass_top",  "id": "0", "source": "data:image/png;base64,iVBORw0KGgoAAAA="},
    {"name": "dirt",       "id": "1", "source": "data:image/png;base64,iVBORw0KGgoBBBB="},
    {"name": "grass_side", "id": "2", "source": "data:image/png;base64,iVBORw0KGgoCCCC="}
  ],
  "elements": [
    {
      "from": [0, 0, 0],
      "to":   [16, 16, 16],
      "faces": {
        "up":    {"uv": [0, 0, 16, 16], "texture": 0},
        "down":  {"uv": [0, 0, 16, 16], "texture": 1},
        "north": {"uv": [0, 0, 16, 16], "texture": 2},
        "south": {"uv": [0, 0, 16, 16], "texture": 2},
        "east":  {"uv": [0, 0, 16, 16], "texture": 2},
        "west":  {"uv": [0, 0, 16, 16], "texture": 2}
      }
    }
  ]
})BB";

// Fetch the .bbmodel importer the loaded plugin registered.
const RegisteredImporter* bbImporter(const PluginManager& pm) {
    for (const auto& imp : pm.importers())
        if (imp.extension == "bbmodel" && imp.fn) return &imp;
    return nullptr;
}

bool hasTexture(const PluginManager& pm, const std::string& id) {
    for (const auto& t : pm.textures())
        if (t.texture_id == id) return !t.data.empty();  // inline bytes recorded
    return false;
}

AtlasTile rect(float u0, float v0, float u1, float v1) {
    AtlasTile t; t.u0 = u0; t.v0 = v0; t.u1 = u1; t.v1 = v1; t.w = 16; t.h = 16;
    return t;
}

constexpr int     kGrid    = 16;
constexpr uint8_t kImported = 200;  // blockbench's kImportPaletteBase (first element)

}  // namespace

TEST(BlockbenchImport, ImportsMaterialsFaceTilesAndVoxelGrid) {
    materialfaces::clearBindings();
    materialfaces::clearTiles();

    PluginManager pm;
    ASSERT_NE(pm.loadPlugin(VOXEL_BLOCKBENCH_PLUGIN_PATH), kInvalidPluginId);
    const RegisteredImporter* imp = bbImporter(pm);
    ASSERT_NE(imp, nullptr);

    std::vector<Voxel> grid(static_cast<size_t>(kGrid) * kGrid * kGrid, Voxel::empty());
    const int rc = imp->fn(reinterpret_cast<const uint8_t*>(kGrassBlock),
                           std::strlen(kGrassBlock), WorldCoord(0, 0, 0),
                           kGrid, grid.data(), imp->user_data);
    ASSERT_EQ(rc, 0);

    // (c) The full-block element filled the whole grid with the imported material.
    for (const Voxel& v : grid) {
        ASSERT_FALSE(v.isEmpty());
        EXPECT_EQ(v.material.palette_index, kImported);
    }

    // (a) The three referenced textures landed in the atlas registry as inline bytes.
    EXPECT_TRUE(hasTexture(pm, "bb_tex_0"));
    EXPECT_TRUE(hasTexture(pm, "bb_tex_1"));
    EXPECT_TRUE(hasTexture(pm, "bb_tex_2"));

    // (b) The material's faces bind to the right tiles. Resolve the registered ids
    // through the atlas (simulated here) and confirm faceTile points each face at
    // its imported texture: top←up (bb_tex_0), bottom←down (bb_tex_1), side←north
    // (bb_tex_2).
    const AtlasTile top  = rect(0.00f, 0.0f, 0.25f, 0.25f);
    const AtlasTile bot  = rect(0.25f, 0.0f, 0.50f, 0.25f);
    const AtlasTile side = rect(0.50f, 0.0f, 0.75f, 0.25f);
    materialfaces::setTileRect("bb_tex_0", top);
    materialfaces::setTileRect("bb_tex_1", bot);
    materialfaces::setTileRect("bb_tex_2", side);

    const materialfaces::FaceTile t  = materialfaces::faceTile(kImported, materialfaces::PosY);
    const materialfaces::FaceTile b  = materialfaces::faceTile(kImported, materialfaces::NegY);
    const materialfaces::FaceTile s  = materialfaces::faceTile(kImported, materialfaces::PosZ);
    ASSERT_TRUE(t.bound);
    ASSERT_TRUE(b.bound);
    ASSERT_TRUE(s.bound);
    EXPECT_FLOAT_EQ(t.tile.u0, top.u0);
    EXPECT_FLOAT_EQ(b.tile.u0, bot.u0);
    EXPECT_FLOAT_EQ(s.tile.u0, side.u0);
}

TEST(BlockbenchImport, MalformedModelFails) {
    PluginManager pm;
    ASSERT_NE(pm.loadPlugin(VOXEL_BLOCKBENCH_PLUGIN_PATH), kInvalidPluginId);
    const RegisteredImporter* imp = bbImporter(pm);
    ASSERT_NE(imp, nullptr);

    std::vector<Voxel> grid(static_cast<size_t>(kGrid) * kGrid * kGrid, Voxel::empty());

    // Not JSON at all → fail (no silently-wrong block).
    const char* junk = "this is not a model";
    EXPECT_NE(imp->fn(reinterpret_cast<const uint8_t*>(junk), std::strlen(junk),
                      WorldCoord(0, 0, 0), kGrid, grid.data(), imp->user_data), 0);

    // Valid JSON but no geometry → fail.
    const char* noElements = R"({"textures": [], "elements": []})";
    EXPECT_NE(imp->fn(reinterpret_cast<const uint8_t*>(noElements), std::strlen(noElements),
                      WorldCoord(0, 0, 0), kGrid, grid.data(), imp->user_data), 0);
}

#endif  // VOXEL_BLOCKBENCH_PLUGIN_PATH

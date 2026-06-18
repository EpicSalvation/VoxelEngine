// Tests for the M15 T3 texture-atlas pipeline.
//
// Two headless halves, no GPU:
//   * TextureAtlasData — the CPU shelf packer (src/renderer/TextureAtlasData).
//     An image "ingests" (as decoded RGBA8 pixels) and its tile resolves to the
//     correct UV region; tiles wrap to new shelves; the empty atlas is empty.
//   * register_texture teardown — a wired-in plugin registers a texture; the
//     PluginManager registry records it under the plugin's owner id and drops it
//     on unload (the §8 owner-tracked teardown contract). The GPU atlas rebuild
//     is runtime-only; here textureManager_ is null, so registration is
//     registry-only and needs no bgfx context.

#include "renderer/TextureAtlasData.h"
#include "core/PluginManager.h"

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

using texture::AtlasTile;
using texture::TextureAtlasData;

namespace {

// A w×h tile filled with one RGBA color.
std::vector<uint8_t> solidTile(uint16_t w, uint16_t h,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
    }
    return px;
}

// True if v is a power of two (atlas dimensions are kept power-of-two).
bool isPow2(uint16_t v) { return v != 0 && (v & (v - 1)) == 0; }

}  // namespace

TEST(TextureAtlasData, EmptyAtlasIsEmpty) {
    TextureAtlasData atlas;
    atlas.pack();
    EXPECT_EQ(atlas.tileCount(), 0u);
    EXPECT_EQ(atlas.width(), 0);
    EXPECT_EQ(atlas.height(), 0);
    EXPECT_TRUE(atlas.pixels().empty());
}

TEST(TextureAtlasData, RejectsZeroAreaTile) {
    TextureAtlasData atlas;
    std::vector<uint8_t> px = solidTile(1, 1, 1, 2, 3, 4);
    EXPECT_EQ(atlas.addTile(px.data(), 0, 1), -1);
    EXPECT_EQ(atlas.addTile(nullptr, 1, 1), -1);
}

TEST(TextureAtlasData, TilesResolveToCorrectUVRegions) {
    TextureAtlasData atlas;
    auto red   = solidTile(2, 2, 255, 0, 0, 255);
    auto green = solidTile(4, 2, 0, 255, 0, 255);
    const int i0 = atlas.addTile(red.data(),   2, 2);
    const int i1 = atlas.addTile(green.data(), 4, 2);
    ASSERT_EQ(i0, 0);
    ASSERT_EQ(i1, 1);
    atlas.pack(/*targetWidth=*/256);

    ASSERT_EQ(atlas.tileCount(), 2u);
    EXPECT_TRUE(isPow2(atlas.width()));
    EXPECT_TRUE(isPow2(atlas.height()));

    const float w = static_cast<float>(atlas.width());
    const float h = static_cast<float>(atlas.height());

    // Both tiles fit on the first shelf: red at x=0, green immediately after at x=2.
    const AtlasTile& t0 = atlas.tile(0);
    EXPECT_FLOAT_EQ(t0.u0, 0.0f);
    EXPECT_FLOAT_EQ(t0.v0, 0.0f);
    EXPECT_FLOAT_EQ(t0.u1, 2.0f / w);
    EXPECT_FLOAT_EQ(t0.v1, 2.0f / h);
    EXPECT_EQ(t0.w, 2);
    EXPECT_EQ(t0.h, 2);

    const AtlasTile& t1 = atlas.tile(1);
    EXPECT_FLOAT_EQ(t1.u0, 2.0f / w);
    EXPECT_FLOAT_EQ(t1.u1, 6.0f / w);
    EXPECT_FLOAT_EQ(t1.v0, 0.0f);

    // The packed pixels carry each tile's color at its placed origin.
    auto pixelAt = [&](uint16_t x, uint16_t y) {
        const uint8_t* p = atlas.pixels().data() +
                           (static_cast<size_t>(y) * atlas.width() + x) * 4;
        return std::vector<uint8_t>{p[0], p[1], p[2], p[3]};
    };
    EXPECT_EQ(pixelAt(0, 0), (std::vector<uint8_t>{255, 0, 0, 255}));  // red
    EXPECT_EQ(pixelAt(2, 0), (std::vector<uint8_t>{0, 255, 0, 255}));  // green
}

TEST(TextureAtlasData, TilesWrapToNewShelf) {
    TextureAtlasData atlas;
    auto a = solidTile(4, 2, 10, 0, 0, 255);
    auto b = solidTile(4, 3, 0, 20, 0, 255);
    atlas.addTile(a.data(), 4, 2);
    atlas.addTile(b.data(), 4, 3);
    atlas.pack(/*targetWidth=*/4);  // width 4 holds exactly one 4-wide tile per shelf

    EXPECT_EQ(atlas.width(), 4);
    // Second tile wrapped below the first: its v0 is the first shelf's height (2)
    // over the atlas height.
    const float h = static_cast<float>(atlas.height());
    EXPECT_FLOAT_EQ(atlas.tile(0).v0, 0.0f);
    EXPECT_FLOAT_EQ(atlas.tile(1).v0, 2.0f / h);
}

// --- register_texture owner-tracked teardown (registry level) ---------------

namespace {
int initRegisterTexture(PluginContext* ctx) {
    ctx->register_texture(ctx, "grass_top", "assets/textures/grass_top.png");
    return 0;
}

bool hasTexture(const PluginManager& pm, const std::string& id) {
    for (const auto& t : pm.textures())
        if (t.texture_id == id) return true;
    return false;
}
}  // namespace

TEST(TextureRegistry, RegisterTextureRecordsAndUnloadTearsDown) {
    PluginManager pm;
    PluginId id = pm.wireInPlugin(initRegisterTexture);
    ASSERT_NE(id, kInvalidPluginId);

    ASSERT_EQ(pm.textures().size(), 1u);
    EXPECT_TRUE(hasTexture(pm, "grass_top"));
    EXPECT_EQ(pm.textures().front().owner, id);
    EXPECT_EQ(pm.textures().front().path, "assets/textures/grass_top.png");

    ASSERT_TRUE(pm.unloadPlugin(id));
    EXPECT_FALSE(hasTexture(pm, "grass_top"));  // gone with its owner
    EXPECT_TRUE(pm.textures().empty());
}

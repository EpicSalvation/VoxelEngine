// Tests for PluginManager: disk load success/failure paths, missing-symbol and
// non-zero-init handling, the duplicate-material overwrite warning, and
// per-plugin unload teardown of the callback registries.
//
// The disk-load tests load real MODULE shared libraries built from
// tests/fixtures/*.cpp; their paths are injected as compile definitions by
// CMake (VOXEL_FIXTURE_GOOD / _NOSYM / _BADINIT).

#include "core/PluginManager.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <string>

namespace {

bool hasMaterial(const PluginManager& pm, const std::string& id) {
    const auto& mats = pm.materials();
    return std::find_if(mats.begin(), mats.end(),
        [&](const RegisteredMaterial& m) { return m.material_id == id; }) != mats.end();
}

// Free-function plugin inits for the wired-in (no .so) registry tests.
int initRegisterA(PluginContext* ctx) {
    MaterialProperties m;
    m.density = 1.0f;
    ctx->register_material(ctx, "dup", m);
    return 0;
}
int initRegisterB(PluginContext* ctx) {
    MaterialProperties m;
    m.density = 2.0f;
    ctx->register_material(ctx, "dup", m);
    return 0;
}
int initRegisterPalette(PluginContext* ctx) {
    MaterialProperties m;
    m.density       = 5.0f;
    m.hardness      = 3.0f;
    m.palette_index = 7;
    ctx->register_material(ctx, "rock", m);
    return 0;
}

}  // namespace

TEST(PluginManager, LoadsFromDiskSuccessfully) {
    PluginManager pm;
    PluginId id = pm.loadPlugin(VOXEL_FIXTURE_GOOD);
    EXPECT_NE(id, kInvalidPluginId);
    EXPECT_TRUE(hasMaterial(pm, "fixture_stone"));
}

TEST(PluginManager, MissingFileFails) {
    PluginManager pm;
    EXPECT_EQ(pm.loadPlugin("definitely_not_a_real_plugin_path"), kInvalidPluginId);
}

TEST(PluginManager, MissingInitSymbolFails) {
    PluginManager pm;
    EXPECT_EQ(pm.loadPlugin(VOXEL_FIXTURE_NOSYM), kInvalidPluginId);
}

TEST(PluginManager, NonZeroInitFailsAndRollsBack) {
    PluginManager pm;
    EXPECT_EQ(pm.loadPlugin(VOXEL_FIXTURE_BADINIT), kInvalidPluginId);
    // The material registered before the non-zero return must not survive.
    EXPECT_FALSE(hasMaterial(pm, "fixture_badmat"));
}

TEST(PluginManager, UnloadRemovesRegistrations) {
    PluginManager pm;
    PluginId id = pm.loadPlugin(VOXEL_FIXTURE_GOOD);
    ASSERT_NE(id, kInvalidPluginId);
    EXPECT_TRUE(hasMaterial(pm, "fixture_stone"));

    EXPECT_TRUE(pm.unloadPlugin(id));
    EXPECT_FALSE(hasMaterial(pm, "fixture_stone"));
    EXPECT_FALSE(pm.unloadPlugin(id));  // already unloaded
}

TEST(PluginManager, DuplicateMaterialOverwrites) {
    PluginManager pm;
    pm.wireInPlugin(initRegisterA);
    pm.wireInPlugin(initRegisterB);

    int count = 0;
    for (const auto& m : pm.materials())
        if (m.material_id == "dup") ++count;

    EXPECT_EQ(count, 1);                                 // overwritten, not duplicated
    EXPECT_FLOAT_EQ(pm.material("dup").density, 2.0f);   // second registration wins (via lookup)
}

TEST(PluginManager, MaterialLookupByIdAndPalette) {
    PluginManager pm;
    PluginId id = pm.wireInPlugin(initRegisterPalette);
    ASSERT_NE(id, kInvalidPluginId);

    // Known id → the registered properties.
    const MaterialProperties rock = pm.material("rock");
    EXPECT_FLOAT_EQ(rock.density, 5.0f);
    EXPECT_FLOAT_EQ(rock.hardness, 3.0f);
    EXPECT_EQ(static_cast<int>(rock.palette_index), 7);

    // Known palette index → the same registered properties.
    EXPECT_FLOAT_EQ(pm.materialForPalette(7).density, 5.0f);

    // Unknown id → a neutral zero-initialized default.
    const MaterialProperties none = pm.material("nonexistent");
    EXPECT_FLOAT_EQ(none.density, 0.0f);
    EXPECT_EQ(static_cast<int>(none.palette_index), 0);

    // Unknown palette index → neutral default carrying the requested index.
    const MaterialProperties p9 = pm.materialForPalette(9);
    EXPECT_FLOAT_EQ(p9.density, 0.0f);
    EXPECT_EQ(static_cast<int>(p9.palette_index), 9);

    // The lookup tracks the registry: after the owning plugin unloads, the id and
    // palette resolve back to the neutral default.
    ASSERT_TRUE(pm.unloadPlugin(id));
    EXPECT_FLOAT_EQ(pm.material("rock").density, 0.0f);
    EXPECT_FLOAT_EQ(pm.materialForPalette(7).density, 0.0f);
}

TEST(PluginManager, WiredInPluginUnloadTearsDown) {
    PluginManager pm;
    PluginId id = pm.wireInPlugin(initRegisterA);
    ASSERT_NE(id, kInvalidPluginId);
    EXPECT_TRUE(hasMaterial(pm, "dup"));

    EXPECT_TRUE(pm.unloadPlugin(id));
    EXPECT_FALSE(hasMaterial(pm, "dup"));
}

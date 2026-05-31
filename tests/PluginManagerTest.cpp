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

    int   count   = 0;
    float density = 0.0f;
    for (const auto& m : pm.materials())
        if (m.material_id == "dup") { ++count; density = m.props.density; }

    EXPECT_EQ(count, 1);              // overwritten, not duplicated
    EXPECT_FLOAT_EQ(density, 2.0f);   // second registration wins
}

TEST(PluginManager, WiredInPluginUnloadTearsDown) {
    PluginManager pm;
    PluginId id = pm.wireInPlugin(initRegisterA);
    ASSERT_NE(id, kInvalidPluginId);
    EXPECT_TRUE(hasMaterial(pm, "dup"));

    EXPECT_TRUE(pm.unloadPlugin(id));
    EXPECT_FALSE(hasMaterial(pm, "dup"));
}

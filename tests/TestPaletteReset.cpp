// Test isolation for the shared visual palette (src/renderer/Palette.h).
//
// The palette is process-global mutable state: a .vox import installs a file's
// colors into it (VoxImporter), which would otherwise leak into later tests —
// e.g. flipping water's translucency and breaking the mesh-builder batch tests.
// This listener restores the default palette before every test so each runs
// against a clean, deterministic table. Registered via a static initializer,
// which runs before gtest_main calls RUN_ALL_TESTS (listeners must be appended
// beforehand).

#include <gtest/gtest.h>

#include "renderer/Palette.h"

namespace {

class PaletteResetListener : public ::testing::EmptyTestEventListener {
    void OnTestStart(const ::testing::TestInfo&) override {
        palette::resetToDefault();
    }
};

const bool kPaletteResetRegistered = [] {
    ::testing::UnitTest::GetInstance()->listeners().Append(new PaletteResetListener);
    return true;
}();

}  // namespace

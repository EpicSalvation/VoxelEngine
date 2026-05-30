#pragma once

#include <cstdint>

// Shared 16-color base palette (ABGR format: 0xAABBGGRR), used by both the
// per-voxel draw path (BgfxRenderer) and the per-chunk mesh builder
// (ChunkMeshData). Index 0 is "empty" and is never rendered. Indices 16-255
// cycle back through the base 16.
namespace palette {

inline constexpr uint32_t kBase[16] = {
    0x00000000,  //  0: empty (not rendered)
    0xffaaaaaa,  //  1: stone
    0xff228b22,  //  2: grass
    0xff2b5a8b,  //  3: dirt
    0xff3be3f4,  //  4: sand
    0xfff08000,  //  5: water (blue)
    0xff003399,  //  6: wood
    0xff00aa44,  //  7: leaves
    0xffffffff,  //  8: snow
    0xff2050e0,  //  9: lava/fire (orange-red)
    0xff333333,  // 10: coal
    0xff6496c8,  // 11: iron ore
    0xff00d0d0,  // 12: gold ore (yellow)
    0xffd0a000,  // 13: diamond (cyan)
    0xff2222cc,  // 14: brick (dark red)
    0xffff00ff,  // 15: debug (magenta)
};

inline uint32_t color(uint8_t idx) { return kBase[idx & 15]; }

}  // namespace palette

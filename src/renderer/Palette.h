#pragma once

#include <cstdint>

// Shared 16-color base palette (ABGR format: 0xAABBGGRR), used by both the
// per-voxel draw path (BgfxRenderer) and the per-chunk mesh builder
// (ChunkMeshData). Index 0 is "empty" and is never rendered. Indices 16-255
// cycle back through the base 16.
//
// The high (alpha) byte is load-bearing: an entry with alpha < 0xff is a
// translucent material. The mesh builder routes its faces into a separate
// transparent batch and the renderer draws them in an alpha-blended pass over
// the opaque geometry (see BgfxRenderer::render). Water is the first such entry.
namespace palette {

inline constexpr uint32_t kBase[16] = {
    0x00000000,  //  0: empty (not rendered)
    0xffaaaaaa,  //  1: stone
    0xff228b22,  //  2: grass
    0xff2b5a8b,  //  3: dirt
    0xff3be3f4,  //  4: sand
    0xb4f08000,  //  5: water (blue, translucent — alpha 0xb4)
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

// True if the material at idx is translucent (palette alpha < fully opaque).
// Translucent faces are batched and drawn separately by the renderer.
inline bool isTranslucent(uint8_t idx) { return (color(idx) >> 24) != 0xffu; }

}  // namespace palette

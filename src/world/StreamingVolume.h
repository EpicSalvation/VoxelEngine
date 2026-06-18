#pragma once

#include <vector>

#include "core/LayerConfig.h"  // StreamingShape
#include "world/Chunk.h"       // ChunkCoord

// Camera-centered streaming residency predicate (M16, L1).
//
// Replaces LODManager's hard-coded "XZ-Chebyshev disc × absolute-Y band" with an
// axis-agnostic volume so the non-block-game worlds the architecture already
// describes stream correctly: a deep-dig world streams downward as the player
// descends (no absolute band to bottom out on — the M8-demo bug), a flying game
// streams a thin shell, a space world an isotropic box.
//
// A box volume reproduces the pre-M16 footprint exactly (so existing configs are
// byte-for-byte unchanged); sphere and shell express isotropic and backdrop-band
// residency without privileging any axis.
//
// This is a value type built per-layer from a LayerDef (LODManager::volumeFor).
// It owns no chunk storage and is pure set/distance math, so it unit-tests with
// no World, no GPU, no window. It answers two questions:
//   - contains(center, c) — is chunk c resident for a camera at chunk `center`?
//   - desired(center)     — the full candidate set to stream in around `center`.
struct StreamingVolume {
    StreamingShape shape = StreamingShape::box;
    int radiusChunks = 0;          // box half-extent / sphere radius / shell outer radius
    int shellThicknessChunks = 1;  // shell only: inner radius = radius − thickness (>= 0)

    // Optional vertical band clamp for the BOX shape only — the "box-volume
    // convenience" the heightmap demos use (LODManager::setVerticalBand). A
    // heightmap that only populates a few chunk-Y indices clamps the otherwise
    // isotropic box so the working set does not load empty sky/underground. The
    // band is camera-independent (absolute chunk-Y); the radius still tracks the
    // camera in Y, so the smaller of the two bounds wins. Sphere and shell ignore
    // the band (they are intrinsically axis-agnostic).
    int yMin = -1'000'000;
    int yMax =  1'000'000;

    // True if chunk c is within this volume centered on camera chunk `center`.
    bool contains(ChunkCoord center, ChunkCoord c) const;

    // Every chunk this volume wants resident around `center`. Enumerated over the
    // shape's bounding cube and filtered by contains(); for the default box (no
    // band) this is exactly the pre-M16 disc×band cube in the same order.
    std::vector<ChunkCoord> desired(ChunkCoord center) const;

    // The same volume grown by `margin` chunks, for eviction hysteresis: the
    // box/sphere radius and the shell's outer edge expand by margin while the
    // shell's inner edge shrinks by margin. The Y band is left as-is — only the
    // camera-relative radius gets hysteresis, matching the pre-M16 behavior.
    StreamingVolume expandedBy(int margin) const;
};

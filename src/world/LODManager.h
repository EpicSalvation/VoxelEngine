#pragma once

#include <string>
#include <vector>

#include "core/LayerConfig.h"
#include "core/Tuning.h"
#include "world/Chunk.h"
#include "world/StreamingVolume.h"

// Per-layer chunk visibility and budget management.
//
// Moved from src/renderer/ to this neutral world tier in M10 so
// DecompositionManager can depend on it without creating a world→renderer edge
// (docs/ARCHITECTURE.md §13).
//
// Each layer carries its own view distance (LayerDef::view_distance_chunks) and
// streaming-volume shape (LayerDef::streaming_shape); LODManager builds the
// layer's StreamingVolume and answers two questions the streaming loop needs each
// frame, now over an axis-agnostic volume rather than an XZ-disc × Y-band (M16, L1):
//   - which chunks should be resident around the camera (desiredChunks)
//   - which resident chunks are now far enough to evict (shouldEvict, with a
//     hysteresis margin beyond the load radius to prevent thrashing)
//
// This is intentionally headless: no GPU, no async, no chunk ownership. It is
// pure set/budget math so it can be unit-tested without a window.
class LODManager {
public:
    explicit LODManager(const LayerConfig& config);

    // Restrict the BOX streaming volume to a vertical band of chunk-Y indices
    // [yMin, yMax] — a box-volume convenience (M16) for vertically bounded
    // heightmap worlds, where a full cube radius would load empty sky/underground.
    // Default is unconstrained (±1 000 000 chunks); the volume further clamps to
    // center.y ± viewDistance so the actual load cube is always finite. Sphere and
    // shell volumes are intrinsically axis-agnostic and ignore the band.
    void setVerticalBand(int yMin, int yMax) { yMin_ = yMin; yMax_ = yMax; }

    // The per-layer StreamingVolume (shape + radii from LayerDef, plus the current
    // box vertical band). Centered on a camera chunk by contains()/desired().
    StreamingVolume volumeFor(const std::string& layerName) const;

    // Load radius (chunks) for the named layer, from its config. 0 if unknown.
    int viewDistanceChunks(const std::string& layerName) const;

    // Eviction radius: load radius plus a hysteresis margin. Chunks beyond this
    // (Chebyshev XZ) distance from the camera should be unloaded.
    int evictDistanceChunks(const std::string& layerName) const;

    // Chunks that should be resident around the camera center: every chunk the
    // layer's StreamingVolume contains (a box cube, a sphere ball, or a shell
    // band), centered on the camera chunk.
    std::vector<ChunkCoord> desiredChunks(ChunkCoord center,
                                          const std::string& layerName) const;

    // True if coord is inside the layer's StreamingVolume centered on center.
    bool withinViewDistance(ChunkCoord center, ChunkCoord coord,
                            const std::string& layerName) const;

    // True if coord should be evicted given the camera center — i.e. it is outside
    // the layer's StreamingVolume grown by the hysteresis margin.
    bool shouldEvict(ChunkCoord center, ChunkCoord coord,
                     const std::string& layerName) const;

    // The eviction hysteresis margin lives centrally in tuning::streaming; kept
    // as a public alias so call sites and tests can reference LODManager::kHysteresisChunks.
    static constexpr int kHysteresisChunks = tuning::streaming::kHysteresisChunks;

private:
    const LayerDef* layer(const std::string& name) const;

    const LayerConfig& config_;
    int yMin_ = -1'000'000;
    int yMax_ =  1'000'000;
};

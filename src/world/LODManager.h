#pragma once

#include <string>
#include <vector>

#include "core/LayerConfig.h"
#include "core/Tuning.h"
#include "world/Chunk.h"

// Per-layer chunk visibility and budget management.
//
// Moved from src/renderer/ to this neutral world tier in M10 so
// DecompositionManager can depend on it without creating a world→renderer edge
// (docs/ARCHITECTURE.md §13).
//
// Each layer carries its own view distance (LayerDef::view_distance_chunks);
// LODManager reads that config and answers two questions the streaming loop
// needs each frame:
//   - which chunks should be resident around the camera (desiredChunks)
//   - which resident chunks are now far enough to evict (shouldEvict, with a
//     hysteresis margin beyond the load radius to prevent thrashing)
//
// This is intentionally headless: no GPU, no async, no chunk ownership. It is
// pure set/budget math so it can be unit-tested without a window.
class LODManager {
public:
    explicit LODManager(const LayerConfig& config);

    // Restrict generated chunks to a vertical band of chunk-Y indices [yMin, yMax].
    // A single-layer heightmap is vertically bounded, so a full cube radius would
    // load empty sky/underground chunks; the band keeps the working set tight.
    // Default is unconstrained (±1 000 000 chunks); desiredChunks further clamps
    // to center.y ± viewDistance so the actual load cube is always finite.
    void setVerticalBand(int yMin, int yMax) { yMin_ = yMin; yMax_ = yMax; }

    // Load radius (chunks) for the named layer, from its config. 0 if unknown.
    int viewDistanceChunks(const std::string& layerName) const;

    // Eviction radius: load radius plus a hysteresis margin. Chunks beyond this
    // (Chebyshev XZ) distance from the camera should be unloaded.
    int evictDistanceChunks(const std::string& layerName) const;

    // Chunks that should be resident around the camera center: a full XZ disc of
    // Chebyshev radius viewDistanceChunks, across the configured vertical band.
    std::vector<ChunkCoord> desiredChunks(ChunkCoord center,
                                          const std::string& layerName) const;

    // True if coord is within the load radius of center (and the vertical band).
    bool withinViewDistance(ChunkCoord center, ChunkCoord coord,
                            const std::string& layerName) const;

    // True if coord should be evicted given the camera center (outside the
    // eviction radius, i.e. load radius + hysteresis).
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

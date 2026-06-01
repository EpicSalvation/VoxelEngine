#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Voxel.h"
#include "Chunk.h"
#include "ChunkCoordMath.h"
#include "core/LayerConfig.h"

// One layer of the world: a streaming cache of fixed-size Chunks keyed by
// ChunkCoord, at the layer's own voxel scale. This is the chunked half of the
// old single-layer World, extracted in M6 so World can hold a stack of layers
// (terminal, composite, immutable) each with its own voxel size and mode.
//
// A Layer owns chunk storage and the world-space single-voxel accessors; it does
// not own a renderer, generator, or thread pool. Chunks are populated on demand
// by a caller-supplied LayerGeneratorFn (see loadChunk) and evicted as the
// camera moves. Coordinate math is double-only (ChunkCoordMath.h) and scaled by
// voxelSizeM(), so a 1 m terminal layer and a 32 m composite layer share the
// same code at different scales.
class Layer {
public:
    using ChunkStore =
        std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash>;

    explicit Layer(const LayerDef& def);

    // ── Identity / config ─────────────────────────────────────────────────
    const std::string&                name()           const { return name_; }
    VoxelMode                         mode()           const { return mode_; }
    double                            voxelSizeM()     const { return voxelSizeM_; }
    int                               chunkSizeVoxels() const { return chunkSizeVoxels_; }
    const std::optional<std::string>& decomposeTo()    const { return decomposeTo_; }

    // ── Chunk streaming ───────────────────────────────────────────────────
    // Generates and inserts the chunk at coord if not already present, invoking
    // the generator with the chunk's world-space origin. Returns the chunk
    // (existing or newly created).
    Chunk* loadChunk(ChunkCoord coord, LayerGeneratorFn generator,
                     void* user_data = nullptr);
    void   unloadChunk(ChunkCoord coord);
    const  Chunk* getChunk(ChunkCoord coord) const;

    // Insert a pre-built chunk (e.g. loaded from disk), replacing any chunk
    // already resident at its coord. Returns the raw pointer.
    Chunk* insertChunk(std::unique_ptr<Chunk> chunk);

    // ── World-space single-voxel access ───────────────────────────────────
    // getVoxel returns Voxel::empty() when the owning chunk is not resident.
    // setVoxel writes only if the owning chunk is resident, returning true; it
    // does not load or generate a chunk.
    Voxel getVoxel(const WorldCoord& pos) const;
    bool  setVoxel(const WorldCoord& pos, const Voxel& voxel);

    // ── Dirty tracking (M5) ───────────────────────────────────────────────
    bool isChunkDirty(ChunkCoord coord) const;
    std::vector<ChunkCoord> dirtyChunkCoords() const;
    void clearChunkDirty(ChunkCoord coord);

    const ChunkStore& chunks() const { return chunks_; }
    ChunkStore&       chunks()       { return chunks_; }

private:
    std::string                name_;
    VoxelMode                  mode_            = VoxelMode::terminal;
    double                     voxelSizeM_      = 1.0;
    int                        chunkSizeVoxels_ = 32;
    std::optional<std::string> decomposeTo_;
    ChunkStore                 chunks_;
};

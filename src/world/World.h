#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "Voxel.h"
#include "Chunk.h"
#include "ChunkCoordMath.h"
#include "core/LayerConfig.h"

// World container.
//
// Two construction modes coexist during M3:
//   - Finite flat grid (M1/M2): fixed width/height/depth, used by the
//     01-single-voxel demo and Renderer::renderWorld.
//   - Chunked terminal layer (M3): a streaming cache of fixed-size Chunks keyed
//     by ChunkCoord, populated on demand by a layer generator and evicted as the
//     camera moves. Multi-layer management (Layer[]) is introduced in M6.
class World {
public:
    using ChunkStore =
        std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash>;

    // Finite flat-grid world (M1/M2).
    World(int width, int height, int depth);

    // Chunked streaming world for a single terminal layer (M3). Reads chunk size
    // and voxel size from the layer definition.
    explicit World(const LayerDef& layer);

    // ── Finite flat-grid API (M1/M2) ──────────────────────────────────────
    // Populates the world using the provided layer generator plugin callback.
    // The generator is called for a single cubic chunk (min side length of the world
    // dimensions) at the world origin. Voxels outside the cubic region stay empty.
    void generateWorld(LayerGeneratorFn generator, void* user_data = nullptr);

    Voxel getVoxel(int x, int y, int z) const;
    void  setVoxel(int x, int y, int z, const Voxel& voxel);

    int getWidth()  const { return width_; }
    int getHeight() const { return height_; }
    int getDepth()  const { return depth_; }

    // ── Chunked streaming API (M3) ────────────────────────────────────────
    // Generates and inserts the chunk at coord if not already present, invoking
    // the generator with the chunk's world-space origin. Returns the chunk
    // (existing or newly created), or nullptr if the world is not chunked.
    Chunk* loadChunk(ChunkCoord coord, LayerGeneratorFn generator,
                     void* user_data = nullptr);
    void   unloadChunk(ChunkCoord coord);
    const  Chunk* getChunk(ChunkCoord coord) const;

    const ChunkStore& chunks() const { return chunks_; }

    bool   isChunked()       const { return chunked_; }
    double voxelSizeM()      const { return voxelSizeM_; }
    int    chunkSizeVoxels() const { return chunkSizeVoxels_; }

private:
    // Finite-grid storage.
    int width_, height_, depth_;
    std::vector<Voxel> voxels_;  // flat row-major: index = x + width*(y + height*z)

    int idx(int x, int y, int z) const { return x + width_ * (y + height_ * z); }

    // Chunked storage.
    bool       chunked_         = false;
    double     voxelSizeM_      = 1.0;
    int        chunkSizeVoxels_ = 32;
    ChunkStore chunks_;
};

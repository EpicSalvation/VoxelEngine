#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Voxel.h"
#include "Chunk.h"
#include "ChunkCoordMath.h"
#include "Layer.h"
#include "core/LayerConfig.h"

// World container.
//
// Two storage models coexist:
//   - Finite flat grid (M1/M2): fixed width/height/depth, used by the
//     01-single-voxel demo and Renderer::renderWorld.
//   - Layer stack (M3+): one or more Layers (see Layer.h), each a streaming
//     chunk cache at its own voxel scale and mode. A single-layer terminal
//     world (M3–M5) is the common case; M6 adds composite + immutable layers.
//
// For backward compatibility the single-layer chunked API (loadChunk, the
// world-space getVoxel/setVoxel, dirty tracking, chunks()) is kept on World and
// forwards to a *primary* layer — the terminal layer the player edits, or the
// first layer if the stack has none. Multi-layer consumers address layers
// explicitly via layer(name) / layers().
class World {
public:
    using ChunkStore = Layer::ChunkStore;

    // Finite flat-grid world (M1/M2).
    World(int width, int height, int depth);

    // Single-layer chunked world (M3): builds one Layer from the layer def and
    // makes it the primary layer.
    explicit World(const LayerDef& layer);

    // Multi-layer chunked world (M6): builds one Layer per def in the config,
    // in config order. The terminal layer is the primary (single-layer-API
    // target); if there is none, the first layer is.
    explicit World(const LayerConfig& config);

    // ── Finite flat-grid API (M1/M2) ──────────────────────────────────────
    void generateWorld(LayerGeneratorFn generator, void* user_data = nullptr);

    Voxel getVoxel(int x, int y, int z) const;
    void  setVoxel(int x, int y, int z, const Voxel& voxel);

    int getWidth()  const { return width_; }
    int getHeight() const { return height_; }
    int getDepth()  const { return depth_; }

    // ── Layer stack (M6) ──────────────────────────────────────────────────
    // Layers in config order. layer(name) returns nullptr if no such layer.
    const std::vector<std::unique_ptr<Layer>>& layers() const { return layers_; }
    Layer*       layer(const std::string& name);
    const Layer* layer(const std::string& name) const;

    // The child (decompose_to) layer of a composite layer, or nullptr if the
    // layer has no decompose_to target or it is not present in the stack.
    Layer*       childLayer(const Layer& parent);
    const Layer* childLayer(const Layer& parent) const;

    // The primary layer the single-layer forwarders target, or nullptr for a
    // finite-grid world.
    Layer*       primaryLayer()       { return primary_; }
    const Layer* primaryLayer() const { return primary_; }

    // True if any layer has a solid voxel at the world position, each sampled at
    // its own voxel scale. Unlike getVoxel (primary layer only) this is the
    // cross-layer query collision and picking use, so the player interacts with
    // composite blocks and immutable backdrop voxels as well as terminal ones.
    bool anySolidAt(const WorldCoord& pos) const;

    // ── Single-layer chunked API (forwards to the primary layer) ──────────
    Chunk* loadChunk(ChunkCoord coord, LayerGeneratorFn generator,
                     void* user_data = nullptr);
    void   unloadChunk(ChunkCoord coord);
    const  Chunk* getChunk(ChunkCoord coord) const;
    Chunk* insertChunk(std::unique_ptr<Chunk> chunk);

    Voxel getVoxel(const WorldCoord& pos) const;
    bool  setVoxel(const WorldCoord& pos, const Voxel& voxel);

    bool isChunkDirty(ChunkCoord coord) const;
    std::vector<ChunkCoord> dirtyChunkCoords() const;
    void clearChunkDirty(ChunkCoord coord);

    const ChunkStore& chunks() const;

    bool   isChunked()       const { return primary_ != nullptr; }
    double voxelSizeM()      const;
    int    chunkSizeVoxels() const;

private:
    // Finite-grid storage.
    int width_, height_, depth_;
    std::vector<Voxel> voxels_;  // flat row-major: index = x + width*(y + height*z)

    int idx(int x, int y, int z) const { return x + width_ * (y + height_ * z); }

    // Layer-stack storage. layers_ owns the layers; primary_ aliases the first.
    std::vector<std::unique_ptr<Layer>> layers_;
    Layer*                              primary_ = nullptr;
};

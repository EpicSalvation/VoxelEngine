#pragma once

// Engine-owned cascade decomposition manager (M10, docs/ARCHITECTURE.md §4/§11).
//
// Through M6–M9 every decomposition was single-step: a composite macro voxel
// revealed its immediate child grid and stopped, and each demo owned and
// hand-rolled the approach-trigger/drain/evict loop. DecompositionManager lifts
// that loop into the engine: it holds one DecompositionState per composite layer,
// drives the approach-trigger → drain → evict pipeline each tick via
// World::childLayer, and returns a per-tick diff that the front-end (demo) uses
// to sync its mesh stores — without the manager touching any GPU resource (§13).
//
// Composite layers are processed coarsest-first so that a fine layer's macro
// voxels are only enqueued after their coarse-layer parent is resident and
// decomposed (the chain requirement: continental decomposes into regional, which
// decomposes into local, which decomposes into terrain — one level per tick,
// each step a single-step pure pass).
//
// The manager fully owns composite-layer chunk streaming (load + evict) so that
// eviction can cascade correctly: when a composite chunk leaves view range, every
// decomposed descendant across all deeper layers is evicted in one consistent
// pass. Terminal and immutable layers are streamed by the demo independently.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Chunk.h"
#include "ChunkCoordMath.h"
#include "DecompositionWorker.h"
#include "LODManager.h"
#include "MacroVoxel.h"

class Layer;
class PluginManager;
class World;
struct LayerConfig;
struct WorldCoord;

// Per-composite-layer diff returned by DecompositionManager::tick().
// The front-end uses these to sync its mesh stores without owning any
// decomposition state.
struct LayerTickDiff {
    std::string compositeLayerName;  // the composite layer that was decomposed
    std::string childLayerName;      // its direct child (decompose_to target)

    // Composite-layer chunks: load/evict events the front-end uses to build/destroy
    // coarse block meshes.
    std::vector<ChunkCoord> newCompChunks;
    std::vector<ChunkCoord> evictedCompChunks;

    // Child-layer chunks: produced/removed by decomposition of macro voxels. The
    // front-end builds fine meshes for newChildChunks and destroys them for evicted.
    std::vector<ChunkCoord> newChildChunks;
    std::vector<ChunkCoord> evictedChildChunks;

    // Macro voxel state changes in the composite layer, used to remesh the composite
    // chunk (the coarse block disappears on decomposition and reappears only when the
    // parent chunk is reloaded — the composite chunk is evicted alongside its children
    // so newlyAtomic coords are informational only; the parent chunk mesh is already
    // destroyed via evictedCompChunks).
    std::vector<chunkmath::VoxelCoord> newlyDecomposed;  // clear the block voxel, remesh comp chunk
    std::vector<chunkmath::VoxelCoord> newlyAtomic;      // parent chunk also evicted
};

class DecompositionManager {
public:
    // world_seed is used to produce deterministic per-macro decomposition seeds
    // (folded with the macro VoxelCoord so each voxel decomposes differently).
    DecompositionManager(World& world, PluginManager& pm,
                         const LayerConfig& config, uint64_t worldSeed,
                         unsigned workerThreads = 0);

    // Drive one tick: stream composite chunks, enqueue approach-triggered
    // decompositions, drain completed jobs, and cascade-evict out-of-view chunks.
    //
    // cameraPos       — world-space camera position (drives LOD streaming and approach)
    // approachRadiusM — radius within which undecomposed macro voxels are enqueued
    // loadPerFrame    — max composite chunks to load per tick (caps hitching)
    // decompPerFrame  — max decomposition jobs to enqueue per tick
    //
    // Returns one LayerTickDiff per composite layer. The caller must:
    //   - Build meshes for all chunks in newCompChunks / newChildChunks
    //   - Destroy meshes for all chunks in evictedCompChunks / evictedChildChunks
    //   - Remesh the composite chunk containing each coord in newlyDecomposed
    //     (the macro voxel's block was cleared; only that composite chunk changes)
    std::vector<LayerTickDiff> tick(const WorldCoord& cameraPos,
                                    double approachRadiusM,
                                    int loadPerFrame   = 4,
                                    int decompPerFrame = 64);

    // State queries.
    bool   isDecomposed(const std::string& layerName, chunkmath::VoxelCoord macro) const;
    bool   isPending   (const std::string& layerName, chunkmath::VoxelCoord macro) const;
    size_t decomposedCount(const std::string& layerName) const;
    size_t pendingCount   (const std::string& layerName) const;
    size_t inFlight() const { return worker_.inFlight(); }

private:
    struct CompositeLayerInfo {
        Layer*             layer;      // the composite layer (owned by World)
        Layer*             childLayer; // its direct child (owned by World)
        int64_t            ratio;      // child voxels per parent voxel edge
        int                parentIdx;  // index in composites_ of the coarser composite, or -1
        DecompositionState state;
    };

    // Build a decomposition job for the given macro voxel in its composite layer.
    // Resolves the recipe (or falls back to the generator) on the main thread so
    // DecompositionWorker never touches PluginManager (ARCHITECTURE §13).
    DecompositionJob makeJob(const CompositeLayerInfo& info,
                             chunkmath::VoxelCoord macro) const;

    // The child-layer chunks that cover one composite macro voxel's subvolume.
    // When parent_voxel_size == child_chunk_world_size this is exactly one chunk;
    // for ratios that are multiples of the chunk size it may be span³ chunks.
    static std::vector<ChunkCoord> childChunksForMacro(chunkmath::VoxelCoord macro,
                                                        const Layer& parent,
                                                        const Layer& child);

    // Recursively evict every decomposed descendant of macro in composite layer ci.
    // Inserts eviction records into diffs. Called before the parent composite chunk
    // is removed from its ChunkStore.
    void cascadeEvict(size_t ci, chunkmath::VoxelCoord macro,
                      std::vector<LayerTickDiff>& diffs);

    World&          world_;
    PluginManager&  pm_;
    LODManager      lod_;
    uint64_t        worldSeed_;
    DecompositionWorker                  worker_;
    std::vector<CompositeLayerInfo>      composites_;   // coarsest-first order
    std::unordered_map<std::string, size_t> compositeIdx_; // layerName → composites_ index
};

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
// pass. Only the ROOT composite layer (the one that is no other composite's
// decompose_to target) is generator-loaded; every non-root composite layer's
// chunks are produced exclusively by its parent's decomposition (ARCHITECTURE §4
// step 5), keeping a single source of truth per chunk. Terminal and immutable
// layers are streamed by the demo independently.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Chunk.h"
#include "ChunkCoordMath.h"
#include "DecompositionWorker.h"
#include "LODManager.h"
#include "MacroVoxel.h"
#include "Voxel.h"

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
    // coarse block meshes. newCompChunks is populated only for the root composite
    // layer (non-root layers' chunks arrive via the parent diff's newChildChunks);
    // evictedCompChunks fires for every composite layer.
    std::vector<ChunkCoord> newCompChunks;
    std::vector<ChunkCoord> evictedCompChunks;

    // Child-layer chunks: produced/removed by decomposition of macro voxels. The
    // front-end builds fine meshes for newChildChunks and destroys them for evicted.
    std::vector<ChunkCoord> newChildChunks;
    std::vector<ChunkCoord> evictedChildChunks;

    // Macro voxel state changes in the composite layer, used to remesh the composite
    // chunk:
    //   newlyDecomposed — the macro's block voxel was cleared (children render
    //                     instead); remesh the owning composite chunk.
    //   newlyAtomic     — the macro returned to atomic. For top-down eviction the
    //                     owning composite chunk was itself evicted (already handled
    //                     via evictedCompChunks, so the remesh lookup finds no chunk
    //                     and is a no-op). For bottom-up re-atomization (the child
    //                     chunks left their own layer's view range) the parent chunk
    //                     is still resident with its block voxel restored — remesh it.
    std::vector<chunkmath::VoxelCoord> newlyDecomposed;
    std::vector<chunkmath::VoxelCoord> newlyAtomic;
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

    // Register a callback invoked before a dirty (player-edited) chunk is evicted
    // from any managed layer. The callback receives a const reference to the chunk
    // and the layer name; it should persist the chunk (e.g. via WorldSave::saveChunk)
    // before returning. Clean chunks are dropped silently (they regenerate
    // deterministically on re-approach). Called from cascadeEvict and the budget
    // enforcement pass (ARCHITECTURE §11).
    using DirtyEvictFn = std::function<void(const Chunk&, const std::string& layerName)>;
    void setDirtyEvictCallback(DirtyEvictFn fn) { onDirtyEvict_ = std::move(fn); }

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
        // Block voxel of each currently decomposed macro, captured just before the
        // drain step clears it, so bottom-up re-atomization can restore the block
        // without re-running the generator. Entries are erased whenever the macro's
        // decomposition state clears (cascade eviction, chunk unload, re-atomize).
        std::unordered_map<chunkmath::VoxelCoord, Voxel, chunkmath::VoxelCoordHash>
            originalVoxel;
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
    // Saves dirty child chunks via onDirtyEvict_ before dropping them.
    // Inserts eviction records into diffs. Called before the parent composite chunk
    // is removed from its ChunkStore (top-down) or by reatomize (bottom-up).
    void cascadeEvict(size_t ci, chunkmath::VoxelCoord macro,
                      std::vector<LayerTickDiff>& diffs);

    // True if macro has a decomposition job in flight, or any decomposed
    // descendant (at any depth) does. Such a macro must not be evicted or
    // re-atomized this tick: a draining job would insert chunks into a state
    // the eviction just cleared.
    bool subtreePending(size_t ci, chunkmath::VoxelCoord macro) const;

    // Collapse a decomposed macro back to its atomic block (the inverse of one
    // decomposition step): cascade-evict every decomposed descendant, then restore
    // the macro's cached block voxel so it renders and collides atomically again.
    // Driven bottom-up: when a child layer's own LOD eviction (or budget pass)
    // wants to drop chunks that exist because this macro decomposed, the macro
    // must return to atomic or the world is left with a hole — the block voxel
    // was cleared at decomposition and nothing else restores it (plan item 2).
    //
    // Skipped (left for a later tick) while the macro's subtree has jobs in
    // flight. Unless force is set, also skipped while any of the macro's child
    // chunks is still within the child layer's eviction radius (span > 1 configs:
    // collapsing would evict in-view children that immediately re-decompose).
    // force is used by the budget pass, which must shed chunks regardless.
    void reatomize(size_t ci, chunkmath::VoxelCoord macro,
                   const WorldCoord& cameraPos, bool force,
                   std::vector<LayerTickDiff>& diffs);

    // Enforce the per-layer resident-chunk budget for composite layer ci, then
    // (if the child is terminal) for its child layer. Evicts farthest-first clean
    // non-pending chunks until the resident count is within the budget. Dirty and
    // near-camera chunks are pinned. Never blocks the main thread. Non-root
    // layers shed chunks by re-atomizing the owning parent macro (forced), so
    // the parent's block voxel is restored rather than leaving a hole.
    void enforceLayerBudget(size_t ci, ChunkCoord camChunkComp,
                            const WorldCoord& cameraPos,
                            std::vector<LayerTickDiff>& diffs);

    // Fire registered ChunkLifecycle hooks for a chunk being created or evicted.
    void fireChunkCreated(const Layer& layer, const Chunk& chunk) const;
    void fireChunkEvicted(const Layer& layer, ChunkCoord cc) const;

    World&          world_;
    PluginManager&  pm_;
    const LayerConfig& config_;
    LODManager      lod_;
    uint64_t        worldSeed_;
    DecompositionWorker                  worker_;
    std::vector<CompositeLayerInfo>      composites_;   // coarsest-first order
    std::unordered_map<std::string, size_t> compositeIdx_; // layerName → composites_ index
    DirtyEvictFn    onDirtyEvict_;  // called before evicting a dirty chunk; may be null
};

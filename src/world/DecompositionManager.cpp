#include "DecompositionManager.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "Layer.h"
#include "Recipe.h"
#include "RecipeResolve.h"
#include "ResolvedRecipe.h"
#include "Voxel.h"
#include "World.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"

// ── Construction ──────────────────────────────────────────────────────────────

DecompositionManager::DecompositionManager(World& world, PluginManager& pm,
                                           const LayerConfig& config,
                                           uint64_t worldSeed,
                                           unsigned workerThreads)
    : world_(world), pm_(pm), lod_(config), worldSeed_(worldSeed),
      worker_(workerThreads) {
    // Build one CompositeLayerInfo per composite layer, coarsest-first
    // (config order is coarsest-to-finest by validation rule).
    for (const LayerDef& def : config.layers()) {
        if (def.mode != VoxelMode::composite) continue;
        Layer* layer = world_.layer(def.name);
        if (!layer) continue;
        Layer* childLyr = def.decompose_to ? world_.layer(*def.decompose_to) : nullptr;
        if (!childLyr) continue;

        CompositeLayerInfo info;
        info.layer     = layer;
        info.childLayer = childLyr;
        info.ratio     = chunkmath::layerRatio(layer->voxelSizeM(), childLyr->voxelSizeM());
        info.parentIdx = -1;  // set below once all are added
        compositeIdx_[def.name] = composites_.size();
        composites_.push_back(std::move(info));
    }

    // Resolve parent indices: the coarser composite that contains each layer.
    for (size_t i = 1; i < composites_.size(); ++i) {
        const std::string& childName = composites_[i].layer->name();
        for (size_t j = 0; j < i; ++j) {
            if (composites_[j].childLayer &&
                composites_[j].childLayer->name() == childName) {
                composites_[i].parentIdx = static_cast<int>(j);
                break;
            }
        }
    }
}

// ── Job building ──────────────────────────────────────────────────────────────

std::vector<ChunkCoord> DecompositionManager::childChunksForMacro(
        chunkmath::VoxelCoord macro, const Layer& parent, const Layer& child) {
    const double parentVoxel    = parent.voxelSizeM();
    const double childChunkSize = child.voxelSizeM() * child.chunkSizeVoxels();
    const int span = std::max(1, static_cast<int>(std::llround(parentVoxel / childChunkSize)));
    const WorldCoord origin = chunkmath::voxelOrigin(macro, parentVoxel);
    const ChunkCoord base   = chunkmath::worldToChunk(origin, child.voxelSizeM(),
                                                      child.chunkSizeVoxels());
    std::vector<ChunkCoord> out;
    out.reserve(static_cast<size_t>(span * span * span));
    for (int z = 0; z < span; ++z)
        for (int y = 0; y < span; ++y)
            for (int x = 0; x < span; ++x)
                out.push_back(ChunkCoord{base.x + x, base.y + y, base.z + z});
    return out;
}

DecompositionJob DecompositionManager::makeJob(const CompositeLayerInfo& info,
                                               chunkmath::VoxelCoord macro) const {
    DecompositionJob job;
    job.macro      = macro;
    job.layerName  = info.layer->name();
    job.childChunks     = childChunksForMacro(macro, *info.layer, *info.childLayer);
    job.childChunkSize  = info.childLayer->chunkSizeVoxels();
    job.childVoxelSizeM = info.childLayer->voxelSizeM();

    const Recipe* recipe = pm_.findRecipe(info.layer->name());
    if (recipe) {
        job.recipe = std::make_shared<ResolvedRecipe>(resolveRecipe(*recipe, pm_));
        job.seed   = voxel_seed_mix(worldSeed_, chunkmath::VoxelCoordHash{}(macro));
        job.ratio  = info.ratio;
        job.macroChildMin = chunkmath::childVoxelMin(macro, info.ratio);
    } else {
        // Synthesized default recipe: run the child layer's generator (M6 path).
        for (const auto& g : pm_.layerGenerators())
            if (g.layer_name == info.childLayer->name()) {
                job.generator = g.fn;
                job.userData  = g.user_data;
                break;
            }
    }
    return job;
}

// ── Cascade eviction ──────────────────────────────────────────────────────────

void DecompositionManager::cascadeEvict(size_t ci, chunkmath::VoxelCoord macro,
                                        std::vector<LayerTickDiff>& diffs) {
    CompositeLayerInfo& info = composites_[ci];
    if (!info.state.isDecomposed(macro)) return;

    Layer& childLayer = *info.childLayer;
    LayerTickDiff& diff = diffs[ci];

    // Remove child chunks from this level's child layer.
    for (const ChunkCoord& cc : childChunksForMacro(macro, *info.layer, childLayer)) {
        if (childLayer.getChunk(cc)) {
            diff.evictedChildChunks.push_back(cc);
            childLayer.unloadChunk(cc);
        }
    }

    // If the child layer is itself composite, recurse into ITS decomposed children.
    auto childIt = compositeIdx_.find(childLayer.name());
    if (childIt != compositeIdx_.end()) {
        size_t childCi = childIt->second;
        CompositeLayerInfo& childInfo = composites_[childCi];
        // Find all macro voxels in the child composite layer that fall within our
        // macro voxel's footprint and are decomposed.
        const chunkmath::VoxelCoord childMin = chunkmath::childVoxelMin(macro, info.ratio);
        for (int64_t dz = 0; dz < info.ratio; ++dz)
            for (int64_t dy = 0; dy < info.ratio; ++dy)
                for (int64_t dx = 0; dx < info.ratio; ++dx) {
                    chunkmath::VoxelCoord childMacro{
                        childMin.x + dx, childMin.y + dy, childMin.z + dz};
                    if (childInfo.state.isDecomposed(childMacro))
                        cascadeEvict(childCi, childMacro, diffs);
                    childInfo.state.clear(childMacro);
                }
    }

    info.state.clear(macro);
    diff.newlyAtomic.push_back(macro);
}

// ── Main tick ─────────────────────────────────────────────────────────────────

std::vector<LayerTickDiff> DecompositionManager::tick(const WorldCoord& cameraPos,
                                                       double approachRadiusM,
                                                       int loadPerFrame,
                                                       int decompPerFrame) {
    // One diff per composite layer, indexed in composites_ order.
    std::vector<LayerTickDiff> diffs(composites_.size());
    for (size_t i = 0; i < composites_.size(); ++i) {
        diffs[i].compositeLayerName = composites_[i].layer->name();
        diffs[i].childLayerName     = composites_[i].childLayer->name();
    }

    // ── 1. Drain completed decomposition jobs ────────────────────────────────
    for (auto& result : worker_.drain()) {
        auto it = compositeIdx_.find(result.layerName);
        if (it == compositeIdx_.end()) continue;
        const size_t ci = it->second;
        CompositeLayerInfo& info = composites_[ci];
        LayerTickDiff& diff = diffs[ci];
        Layer& childLayer = *info.childLayer;

        for (auto& chunk : result.chunks) {
            const ChunkCoord cc = chunk->coord();
            childLayer.insertChunk(std::move(chunk));
            diff.newChildChunks.push_back(cc);
        }
        info.state.markDecomposed(result.macro);
        diff.newlyDecomposed.push_back(result.macro);

        // Clear the atomic block voxel in the composite layer so the composite
        // chunk mesh no longer shows it (the child voxels render instead).
        info.layer->setVoxel(
            chunkmath::voxelCenter(result.macro, info.layer->voxelSizeM()),
            Voxel::empty());
    }

    // ── 2. Stream composite-layer chunks (load + evict) ──────────────────────
    //    Processed coarsest-first. A finer composite layer only loads chunks
    //    within its LOD budget; eviction is done after loading so the newly
    //    resident chunks are not immediately evicted.
    for (size_t ci = 0; ci < composites_.size(); ++ci) {
        CompositeLayerInfo& info = composites_[ci];
        Layer& layer = *info.layer;
        LayerTickDiff& diff = diffs[ci];

        const ChunkCoord camChunk =
            chunkmath::worldToChunk(cameraPos, layer.voxelSizeM(), layer.chunkSizeVoxels());

        // Load: use the layer's generator to fill composite chunks.
        LayerGeneratorFn genFn = nullptr;
        void* genUD = nullptr;
        for (const auto& g : pm_.layerGenerators())
            if (g.layer_name == layer.name()) { genFn = g.fn; genUD = g.user_data; break; }

        int loaded = 0;
        for (const ChunkCoord& cc : lod_.desiredChunks(camChunk, layer.name())) {
            if (layer.getChunk(cc)) continue;
            if (loaded >= loadPerFrame) break;
            if (Chunk* chunk = layer.loadChunk(cc, genFn, genUD)) {
                diff.newCompChunks.push_back(cc);
                ++loaded;
                (void)chunk;
            }
        }

        // Evict: composite chunks that have moved out of the eviction radius.
        // A chunk with pending jobs is pinned to avoid draining into a removed slot.
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : layer.chunks())
            if (lod_.shouldEvict(camChunk, kv.first, layer.name()))
                toEvict.push_back(kv.first);

        for (const ChunkCoord& cc : toEvict) {
            // Pin chunks that contain pending macro voxels (job in flight).
            const int n = layer.chunkSizeVoxels();
            bool anyPending = false;
            for (int z = 0; z < n && !anyPending; ++z)
                for (int y = 0; y < n && !anyPending; ++y)
                    for (int x = 0; x < n && !anyPending; ++x)
                        if (info.state.isPending(chunkmath::chunkLocalToVoxel(cc, x, y, z, n)))
                            anyPending = true;
            if (anyPending) continue;

            // Cascade-evict every decomposed child of every macro voxel in this chunk.
            const int n2 = layer.chunkSizeVoxels();
            for (int z = 0; z < n2; ++z)
                for (int y = 0; y < n2; ++y)
                    for (int x = 0; x < n2; ++x) {
                        chunkmath::VoxelCoord macro = chunkmath::chunkLocalToVoxel(cc, x, y, z, n2);
                        if (info.state.isDecomposed(macro))
                            cascadeEvict(ci, macro, diffs);
                        else
                            info.state.clear(macro);
                    }

            diff.evictedCompChunks.push_back(cc);
            layer.unloadChunk(cc);
        }
    }

    // ── 3. Approach-trigger: enqueue undecomposed macro voxels near camera ───
    //    Coarsest-first. A fine composite layer's macro voxels are only enqueued
    //    if their coarser ancestor is already decomposed (the chain requirement).
    int enqueued = 0;
    for (size_t ci = 0; ci < composites_.size() && enqueued < decompPerFrame; ++ci) {
        CompositeLayerInfo& info = composites_[ci];
        Layer& layer = *info.layer;
        const double voxelSize = layer.voxelSizeM();

        const chunkmath::VoxelCoord camVoxel = chunkmath::worldToVoxel(cameraPos, voxelSize);
        const int64_t halfR =
            static_cast<int64_t>(std::ceil(approachRadiusM / voxelSize)) + 1;

        for (int64_t dz = -halfR; dz <= halfR && enqueued < decompPerFrame; ++dz) {
            for (int64_t dy = -halfR; dy <= halfR && enqueued < decompPerFrame; ++dy) {
                for (int64_t dx = -halfR; dx <= halfR && enqueued < decompPerFrame; ++dx) {
                    const chunkmath::VoxelCoord macro{
                        camVoxel.x + dx, camVoxel.y + dy, camVoxel.z + dz};

                    if (!info.state.needsDecompose(macro)) continue;

                    // For layers that are not the coarsest, the parent composite
                    // must already be decomposed and resident (chain requirement).
                    if (info.parentIdx >= 0) {
                        const CompositeLayerInfo& parent = composites_[info.parentIdx];
                        const int64_t parentRatio = chunkmath::layerRatio(
                            parent.layer->voxelSizeM(), voxelSize);
                        const chunkmath::VoxelCoord parentMacro =
                            chunkmath::childToParentVoxel(macro, parentRatio);
                        if (!parent.state.isDecomposed(parentMacro)) continue;
                    }

                    // Distance check (sphere, not cube).
                    const WorldCoord macroCenter = chunkmath::voxelCenter(macro, voxelSize);
                    const glm::dvec3 delta = macroCenter.value - cameraPos.value;
                    const double distSq = delta.x*delta.x + delta.y*delta.y + delta.z*delta.z;
                    if (distSq > approachRadiusM * approachRadiusM) continue;

                    // The macro voxel must be solid in the composite layer
                    // (a cleared voxel was already decomposed and its block removed).
                    if (layer.getVoxel(macroCenter).isEmpty()) continue;

                    if (info.state.markPending(macro)) {
                        worker_.enqueue(makeJob(info, macro));
                        ++enqueued;
                    }
                }
            }
        }
    }

    return diffs;
}

// ── State queries ─────────────────────────────────────────────────────────────

bool DecompositionManager::isDecomposed(const std::string& layerName,
                                        chunkmath::VoxelCoord macro) const {
    auto it = compositeIdx_.find(layerName);
    return it != compositeIdx_.end() && composites_[it->second].state.isDecomposed(macro);
}

bool DecompositionManager::isPending(const std::string& layerName,
                                     chunkmath::VoxelCoord macro) const {
    auto it = compositeIdx_.find(layerName);
    return it != compositeIdx_.end() && composites_[it->second].state.isPending(macro);
}

size_t DecompositionManager::decomposedCount(const std::string& layerName) const {
    auto it = compositeIdx_.find(layerName);
    return it != compositeIdx_.end() ? composites_[it->second].state.decomposedCount() : 0;
}

size_t DecompositionManager::pendingCount(const std::string& layerName) const {
    auto it = compositeIdx_.find(layerName);
    return it != compositeIdx_.end() ? composites_[it->second].state.pendingCount() : 0;
}

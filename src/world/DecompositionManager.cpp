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

namespace {

// Squared distance from a point to the closest point of a macro voxel's AABB.
// Zero when the point is inside the voxel. This — not center distance — is what
// "within the approach radius" means: a center test under-triggers by up to
// voxelSize*√3/2, stalling the cascade at the single block under the camera.
double macroSurfaceDistSq(chunkmath::VoxelCoord macro, double voxelSize,
                          const WorldCoord& point) {
    const glm::dvec3 lo = chunkmath::voxelOrigin(macro, voxelSize).value;
    const glm::dvec3 closest = glm::clamp(point.value, lo, lo + glm::dvec3(voxelSize));
    const glm::dvec3 delta = closest - point.value;
    return glm::dot(delta, delta);
}

}  // namespace

// ── Construction ──────────────────────────────────────────────────────────────

DecompositionManager::DecompositionManager(World& world, PluginManager& pm,
                                           const LayerConfig& config,
                                           uint64_t worldSeed,
                                           unsigned workerThreads)
    : world_(world), pm_(pm), config_(config), lod_(config), worldSeed_(worldSeed),
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
        // ── Build inherited param set (M10 cross-step seed cascade) ──────────
        // Start with engine-reserved __-namespaced positional/material params
        // describing this macro voxel (weakest — overridden by any ancestor key).
        const WorldCoord macroCenter =
            chunkmath::voxelCenter(macro, info.layer->voxelSizeM());
        RecipeParamValue altParam;
        altParam.key    = "__altitude";
        altParam.kind   = RecipeParamKind::Number;
        altParam.number = macroCenter.value.y;
        RecipeParamValue matParam;
        matParam.key    = "__parent_material";
        matParam.kind   = RecipeParamKind::Number;
        matParam.number = static_cast<double>(
            info.layer->getVoxel(macroCenter).material.palette_index);

        std::vector<RecipeParamValue> inherited{altParam, matParam};

        // Merge ancestor recipe seed_parameters root → immediate parent.
        // Each ancestor's params override the preceding (and the reserved base).
        // The inherited set is a pure function of (world_seed, ancestor coords,
        // recipes), so it re-derives identically after a clean evict (§4/§11).
        const size_t ci = compositeIdx_.at(info.layer->name());
        std::vector<int> ancestorIdxs;
        for (int ai = composites_[ci].parentIdx; ai >= 0;
             ai = composites_[ai].parentIdx)
            ancestorIdxs.push_back(ai);
        std::reverse(ancestorIdxs.begin(), ancestorIdxs.end());  // root first
        for (int ai : ancestorIdxs) {
            const Recipe* ancestorRecipe =
                pm_.findRecipe(composites_[ai].layer->name());
            if (ancestorRecipe)
                inherited = mergeRecipeParams(
                    inherited, ancestorRecipe->seed_parameters);
        }

        job.recipe = std::make_shared<ResolvedRecipe>(
            resolveRecipe(*recipe, pm_, inherited));
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

void DecompositionManager::fireChunkCreated(const Layer& layer, const Chunk& chunk) const {
    for (const auto& hook : pm_.chunkCreatedHooks())
        if (hook.layer_name == layer.name())
            hook.fn(chunk.origin(), hook.user_data);
}

void DecompositionManager::fireChunkEvicted(const Layer& layer, ChunkCoord cc) const {
    const Chunk* chunk = layer.getChunk(cc);
    if (!chunk) return;
    for (const auto& hook : pm_.chunkEvictedHooks())
        if (hook.layer_name == layer.name())
            hook.fn(chunk->origin(), hook.user_data);
}

void DecompositionManager::cascadeEvict(size_t ci, chunkmath::VoxelCoord macro,
                                        std::vector<LayerTickDiff>& diffs) {
    CompositeLayerInfo& info = composites_[ci];
    if (!info.state.isDecomposed(macro)) return;

    Layer& childLayer = *info.childLayer;
    LayerTickDiff& diff = diffs[ci];

    // Remove child chunks from this level's child layer.
    // Only terminal-layer chunks can carry player edits worth saving; composite
    // chunks may be flagged dirty by the setVoxel(empty) call in the drain step
    // (a rendered-state update, not a player edit) and must NOT be persisted.
    const bool childIsTerminal = (childLayer.mode() == VoxelMode::terminal);
    for (const ChunkCoord& cc : childChunksForMacro(macro, *info.layer, childLayer)) {
        const Chunk* chunk = childLayer.getChunk(cc);
        if (chunk) {
            // Save dirty terminal chunks before dropping them (ARCHITECTURE §11).
            if (childIsTerminal && chunk->dirty() && onDirtyEvict_)
                onDirtyEvict_(*chunk, childLayer.name());
            fireChunkEvicted(childLayer, cc);
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
                    childInfo.originalVoxel.erase(childMacro);
                }
    }

    info.state.clear(macro);
    info.originalVoxel.erase(macro);
    diff.newlyAtomic.push_back(macro);
}

// ── Bottom-up re-atomization ──────────────────────────────────────────────────

bool DecompositionManager::subtreePending(size_t ci, chunkmath::VoxelCoord macro) const {
    const CompositeLayerInfo& info = composites_[ci];
    if (info.state.isPending(macro)) return true;
    if (!info.state.isDecomposed(macro)) return false;

    auto childIt = compositeIdx_.find(info.childLayer->name());
    if (childIt == compositeIdx_.end()) return false;  // child is terminal

    const chunkmath::VoxelCoord childMin = chunkmath::childVoxelMin(macro, info.ratio);
    for (int64_t dz = 0; dz < info.ratio; ++dz)
        for (int64_t dy = 0; dy < info.ratio; ++dy)
            for (int64_t dx = 0; dx < info.ratio; ++dx)
                if (subtreePending(childIt->second,
                        {childMin.x + dx, childMin.y + dy, childMin.z + dz}))
                    return true;
    return false;
}

void DecompositionManager::reatomize(size_t ci, chunkmath::VoxelCoord macro,
                                     const WorldCoord& cameraPos, bool force,
                                     std::vector<LayerTickDiff>& diffs) {
    CompositeLayerInfo& info = composites_[ci];
    if (!info.state.isDecomposed(macro)) return;
    if (subtreePending(ci, macro)) return;  // jobs in flight below; retry next tick

    if (!force) {
        // Collapse only once ALL of the macro's child chunks have left the child
        // layer's eviction radius. With span > 1 some siblings may still be in
        // view; collapsing then would evict chunks that immediately re-decompose.
        const Layer& childLayer = *info.childLayer;
        const ChunkCoord childCam = chunkmath::worldToChunk(
            cameraPos, childLayer.voxelSizeM(), childLayer.chunkSizeVoxels());
        for (const ChunkCoord& cc : childChunksForMacro(macro, *info.layer, childLayer))
            if (!lod_.shouldEvict(childCam, cc, childLayer.name()))
                return;
    }

    // Copy the cached block voxel before cascadeEvict erases the cache entry.
    Voxel restored = Voxel::empty();
    bool haveVoxel = false;
    if (auto it = info.originalVoxel.find(macro); it != info.originalVoxel.end()) {
        restored  = it->second;
        haveVoxel = true;
    }

    cascadeEvict(ci, macro, diffs);  // also pushes macro into newlyAtomic

    // Restore the block so the macro renders (and collides) atomically again.
    // The owning composite chunk is still resident — only its children left view
    // range — and the front-end remeshes it via the newlyAtomic diff.
    if (haveVoxel)
        info.layer->setVoxel(
            chunkmath::voxelCenter(macro, info.layer->voxelSizeM()), restored);
}

// ── Budget enforcement ────────────────────────────────────────────────────────

void DecompositionManager::enforceLayerBudget(size_t ci, ChunkCoord camChunkComp,
                                               const WorldCoord& cameraPos,
                                               std::vector<LayerTickDiff>& diffs) {
    CompositeLayerInfo& info = composites_[ci];
    Layer& layer = *info.layer;
    const LayerDef* def = config_.findLayer(layer.name());
    if (!def || def->resident_chunk_budget <= 0) return;

    const int budget = def->resident_chunk_budget;
    int current = static_cast<int>(layer.chunks().size());
    if (current <= budget) return;

    // Collect evictable (clean, non-pending) chunks with their camera distance.
    struct Candidate { ChunkCoord coord; int dist; };
    std::vector<Candidate> candidates;
    candidates.reserve(layer.chunks().size());
    const int n = layer.chunkSizeVoxels();
    for (const auto& kv : layer.chunks()) {
        const ChunkCoord& cc = kv.first;
        if (kv.second->dirty()) continue;  // pin dirty

        bool anyPending = false;
        for (int z = 0; z < n && !anyPending; ++z)
            for (int y = 0; y < n && !anyPending; ++y)
                for (int x = 0; x < n && !anyPending; ++x)
                    if (subtreePending(ci, chunkmath::chunkLocalToVoxel(cc, x, y, z, n)))
                        anyPending = true;
        if (anyPending) continue;

        int dist = std::max({std::abs(cc.x - camChunkComp.x),
                             std::abs(cc.y - camChunkComp.y),
                             std::abs(cc.z - camChunkComp.z)});
        candidates.push_back({cc, dist});
    }
    // Farthest first.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b){ return a.dist > b.dist; });

    LayerTickDiff& diff = diffs[ci];
    for (const auto& cand : candidates) {
        // A re-atomization can remove several sibling chunks at once, so track
        // the resident count from the store rather than hand-decrementing.
        current = static_cast<int>(layer.chunks().size());
        if (current <= budget) break;
        if (!layer.getChunk(cand.coord)) continue;  // collapsed via a sibling

        if (info.parentIdx >= 0) {
            // Non-root layer: shed this chunk by collapsing its parent macro
            // (forced — budget pressure overrides the view-range check), which
            // restores the parent's block voxel instead of leaving a hole.
            const CompositeLayerInfo& parent = composites_[info.parentIdx];
            const chunkmath::VoxelCoord parentMacro = chunkmath::childToParentVoxel(
                chunkmath::chunkLocalToVoxel(cand.coord, 0, 0, 0, n), parent.ratio);
            if (parent.state.isDecomposed(parentMacro)) {
                reatomize(static_cast<size_t>(info.parentIdx), parentMacro,
                          cameraPos, /*force=*/true, diffs);
                continue;
            }
            // Orphan: fall through to a direct unload.
        }

        // Cascade-evict all decomposed descendants.
        for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    chunkmath::VoxelCoord macro = chunkmath::chunkLocalToVoxel(
                        cand.coord, x, y, z, n);
                    if (info.state.isDecomposed(macro)) {
                        cascadeEvict(ci, macro, diffs);
                    } else {
                        info.state.clear(macro);
                        info.originalVoxel.erase(macro);
                    }
                }
        fireChunkEvicted(layer, cand.coord);
        diff.evictedCompChunks.push_back(cand.coord);
        layer.unloadChunk(cand.coord);
    }
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
            Chunk* inserted = childLayer.insertChunk(std::move(chunk));
            if (inserted) fireChunkCreated(childLayer, *inserted);
            diff.newChildChunks.push_back(cc);
        }
        info.state.markDecomposed(result.macro);
        diff.newlyDecomposed.push_back(result.macro);

        // Clear the atomic block voxel in the composite layer so the composite
        // chunk mesh no longer shows it (the child voxels render instead), caching
        // it first so bottom-up re-atomization can restore the block (plan item 2).
        const WorldCoord macroCenter =
            chunkmath::voxelCenter(result.macro, info.layer->voxelSizeM());
        info.originalVoxel[result.macro] = info.layer->getVoxel(macroCenter);
        info.layer->setVoxel(macroCenter, Voxel::empty());
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

        // Load: only the ROOT composite layer (no composite parent) is streamed
        // from its generator. A non-root composite layer's chunks come exclusively
        // from its parent's decomposition (ARCHITECTURE §4 step 5) — generator-
        // streaming them too would create a second source of truth for the same
        // chunks, racing decomposition output (insertChunk silently overwrites)
        // and rendering fine content under macro blocks that never decomposed.
        if (info.parentIdx < 0) {
            LayerGeneratorFn genFn = nullptr;
            void* genUD = nullptr;
            for (const auto& g : pm_.layerGenerators())
                if (g.layer_name == layer.name()) { genFn = g.fn; genUD = g.user_data; break; }

            int loaded = 0;
            for (const ChunkCoord& cc : lod_.desiredChunks(camChunk, layer.name())) {
                if (layer.getChunk(cc)) continue;
                if (loaded >= loadPerFrame) break;
                if (Chunk* chunk = layer.loadChunk(cc, genFn, genUD)) {
                    fireChunkCreated(layer, *chunk);
                    diff.newCompChunks.push_back(cc);
                    ++loaded;
                }
            }
        }

        // Evict: composite chunks that have moved out of the eviction radius.
        // A chunk whose macros have jobs in flight (at any depth) is pinned to
        // avoid draining into a state the eviction just cleared.
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : layer.chunks())
            if (lod_.shouldEvict(camChunk, kv.first, layer.name()))
                toEvict.push_back(kv.first);

        const int n = layer.chunkSizeVoxels();
        for (const ChunkCoord& cc : toEvict) {
            if (!layer.getChunk(cc)) continue;  // already collapsed via a sibling

            bool anyPending = false;
            for (int z = 0; z < n && !anyPending; ++z)
                for (int y = 0; y < n && !anyPending; ++y)
                    for (int x = 0; x < n && !anyPending; ++x)
                        if (subtreePending(ci, chunkmath::chunkLocalToVoxel(cc, x, y, z, n)))
                            anyPending = true;
            if (anyPending) continue;

            if (info.parentIdx >= 0) {
                // Non-root layer: this chunk exists because a parent macro
                // decomposed. Dropping it directly would leave that macro's
                // cleared block as a hole; instead collapse the parent macro
                // back to atomic (which evicts this chunk, all its siblings,
                // and every deeper descendant in one consistent pass).
                const CompositeLayerInfo& parent = composites_[info.parentIdx];
                // Integer-exact: this chunk's min voxel, collapsed to the parent
                // grid (parent.ratio is parent-voxel : this-layer-voxel).
                const chunkmath::VoxelCoord parentMacro = chunkmath::childToParentVoxel(
                    chunkmath::chunkLocalToVoxel(cc, 0, 0, 0, n), parent.ratio);
                if (parent.state.isDecomposed(parentMacro)) {
                    // Pin while the parent macro is still inside the approach
                    // radius: collapsing it would just re-trigger decomposition
                    // next tick (decompose/collapse thrash when a child layer's
                    // eviction radius is smaller than the approach radius).
                    if (macroSurfaceDistSq(parentMacro, parent.layer->voxelSizeM(),
                                           cameraPos) >
                        approachRadiusM * approachRadiusM) {
                        reatomize(static_cast<size_t>(info.parentIdx), parentMacro,
                                  cameraPos, /*force=*/false, diffs);
                    }
                    continue;
                }
                // Orphan (no decomposed parent on record): fall through and
                // unload directly so it cannot stay resident forever.
            }

            // Root layer (or orphan): cascade-evict every decomposed macro in
            // this chunk, then drop the chunk itself.
            for (int z = 0; z < n; ++z)
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x) {
                        chunkmath::VoxelCoord macro = chunkmath::chunkLocalToVoxel(cc, x, y, z, n);
                        if (info.state.isDecomposed(macro)) {
                            cascadeEvict(ci, macro, diffs);
                        } else {
                            info.state.clear(macro);
                            info.originalVoxel.erase(macro);
                        }
                    }

            fireChunkEvicted(layer, cc);
            diff.evictedCompChunks.push_back(cc);
            layer.unloadChunk(cc);
        }

        // ── 2b. Budget enforcement ───────────────────────────────────────────
        // If the layer's resident set still exceeds its configured cap after the
        // normal LOD eviction, evict farthest-first clean non-pending chunks.
        enforceLayerBudget(ci, camChunk, cameraPos, diffs);
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

                    // Distance check: camera to the macro voxel's AABB surface
                    // (see macroSurfaceDistSq).
                    if (macroSurfaceDistSq(macro, voxelSize, cameraPos) >
                        approachRadiusM * approachRadiusM)
                        continue;

                    // The macro voxel must be solid in the composite layer
                    // (a cleared voxel was already decomposed and its block removed).
                    const WorldCoord macroCenter = chunkmath::voxelCenter(macro, voxelSize);
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

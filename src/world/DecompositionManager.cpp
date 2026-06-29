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
#include "core/Profiler.h"

namespace {

// Squared distance from a point to the closest point of a cube AABB (zero when
// the point is inside).
double aabbDistSq(const glm::dvec3& lo, double size, const WorldCoord& point) {
    const glm::dvec3 closest = glm::clamp(point.value, lo, lo + glm::dvec3(size));
    const glm::dvec3 delta = closest - point.value;
    return glm::dot(delta, delta);
}

// Squared distance from a point to a macro voxel's AABB surface. This — not
// center distance — is what "within the approach radius" means: a center test
// under-triggers by up to voxelSize*√3/2, stalling the cascade at the single
// block under the camera.
double macroSurfaceDistSq(chunkmath::VoxelCoord macro, double voxelSize,
                          const WorldCoord& point) {
    return aabbDistSq(chunkmath::voxelOrigin(macro, voxelSize).value, voxelSize, point);
}

// Squared distance from a point to a whole chunk's AABB surface, for rejecting
// chunks wholesale before scanning their voxels.
double chunkSurfaceDistSq(ChunkCoord cc, double voxelSize, int chunkSizeVoxels,
                          const WorldCoord& point) {
    return aabbDistSq(chunkmath::chunkOrigin(cc, voxelSize, chunkSizeVoxels).value,
                      voxelSize * chunkSizeVoxels, point);
}

// Flatten owning RecipeParamValues into the POD RecipeParam array a NoiseFn
// consumes. The char* point into the source vector's strings, which must outlive
// the array — they do: the DerivedOccupancyGen owns both for the manager's life.
std::vector<RecipeParam> flattenParams(const std::vector<RecipeParamValue>& params) {
    std::vector<RecipeParam> out;
    out.reserve(params.size());
    for (const RecipeParamValue& p : params) {
        RecipeParam rp;
        rp.key    = p.key.c_str();
        rp.kind   = p.kind;
        rp.number = p.number;
        rp.text   = p.text.empty() ? nullptr : p.text.c_str();
        out.push_back(rp);
    }
    return out;
}

// The material an atomic (undecomposed) macro renders/collides as until it
// decomposes: the highest-weight material of the recipe's interior distribution,
// resolved through the M8 lookup. Falls back to a neutral solid block when the
// recipe paints no interior (a degenerate occupancy-only recipe).
MaterialProperties dominantInteriorMaterial(const Recipe& recipe, const PluginManager& pm) {
    const MaterialWeightValue* best = nullptr;
    for (const MaterialWeightValue& m : recipe.interior.materials)
        if (!best || m.weight > best->weight) best = &m;
    if (best) return pm.material(best->material_id);
    MaterialProperties m;
    m.density = 1000.0f;
    m.structural_strength = 0.5f;
    m.hardness = 0.5f;
    m.palette_index = 1;
    return m;
}

}  // namespace

// Synthesized coarse-occupancy generator (M18.5). For each macro voxel in the
// chunk, sample the recipe's carve field over the macro's child-resolution
// footprint with the SAME per-macro occupancy seed decomposition uses; mark the
// macro solid (its block material) iff any child cell is solid, else empty. This
// makes coarse occupancy an exact superset of fine occupancy by construction,
// removing the hand-written, must-stay-in-sync coarse generator (ARCHITECTURE §4).
void DecompositionManager::derivedOccupancyGenerator(WorldCoord chunkOrigin, int gridSize,
                                                     Voxel* out, void* user_data) {
    const DerivedOccupancyGen& d = *static_cast<const DerivedOccupancyGen*>(user_data);
    if (!d.occ.noise || d.ratio <= 0) return;  // nothing to derive: leave all empty

    for (int z = 0; z < gridSize; ++z)
        for (int y = 0; y < gridSize; ++y)
            for (int x = 0; x < gridSize; ++x) {
                const double mx0 = chunkOrigin.value.x + x * d.macroVoxelSizeM;
                const double my0 = chunkOrigin.value.y + y * d.macroVoxelSizeM;
                const double mz0 = chunkOrigin.value.z + z * d.macroVoxelSizeM;
                // The macro VoxelCoord (its center disambiguates the floor) and the
                // per-macro occupancy seed, matching makeJob/fillChildChunk exactly.
                const WorldCoord macroCenter(mx0 + 0.5 * d.macroVoxelSizeM,
                                             my0 + 0.5 * d.macroVoxelSizeM,
                                             mz0 + 0.5 * d.macroVoxelSizeM);
                const chunkmath::VoxelCoord macro =
                    chunkmath::worldToVoxel(macroCenter, d.macroVoxelSizeM);
                const uint64_t decompSeed =
                    voxel_seed_mix(d.worldSeed, chunkmath::VoxelCoordHash{}(macro));
                const uint64_t occSeed = voxel_seed_mix(decompSeed, kRecipeOccupancySalt);

                bool present = false;
                for (int cz = 0; cz < d.ratio && !present; ++cz)
                    for (int cy = 0; cy < d.ratio && !present; ++cy)
                        for (int cx = 0; cx < d.ratio && !present; ++cx) {
                            const WorldCoord cc(mx0 + (cx + 0.5) * d.childVoxelSizeM,
                                                my0 + (cy + 0.5) * d.childVoxelSizeM,
                                                mz0 + (cz + 0.5) * d.childVoxelSizeM);
                            if (d.occ.noise(cc, occSeed, d.flat.data(), d.flat.size(),
                                            d.occ.noiseUser) >= d.occ.threshold)
                                present = true;
                        }
                out[x + gridSize * (y + gridSize * z)] =
                    present ? Voxel{d.block} : Voxel::empty();
            }
}

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
        info.decomposeRadiusM = def.decompose_distance_m.value_or(0.0);
        compositeIdx_[def.name] = composites_.size();
        composites_.push_back(std::move(info));
    }

    // Immutable layers (M16, L5): streamed under their own StreamingVolume +
    // budget like composite/terminal layers, instead of generated-once-and-fully-
    // resident. Resolve each layer's generator once (plugins register before the
    // manager exists). Layers without a generator are skipped — nothing to stream.
    for (const LayerDef& def : config.layers()) {
        if (def.mode != VoxelMode::immutable) continue;
        Layer* layer = world_.layer(def.name);
        if (!layer) continue;
        ImmutableLayerInfo info;
        info.layer = layer;
        for (const auto& g : pm_.layerGenerators())
            if (g.layer_name == def.name) { info.genFn = g.fn; info.genUD = g.user_data; break; }
        if (!info.genFn) continue;
        immutables_.push_back(info);
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

    // Cache each layer's ancestor chain (root-first) and the merged ancestor
    // seed_parameters (M10 cross-step cascade). Both are pure functions of the
    // layer topology and the registered recipes, which are fixed by the time a
    // manager exists (plugins register recipes at init) — makeJob would
    // otherwise redo this walk and merge for every enqueued macro.
    for (auto& info : composites_) {
        const size_t selfIdx = compositeIdx_.at(info.layer->name());
        for (int ai = composites_[selfIdx].parentIdx; ai >= 0;
             ai = composites_[ai].parentIdx)
            info.ancestorIdxs.push_back(ai);
        std::reverse(info.ancestorIdxs.begin(), info.ancestorIdxs.end());

        for (int ai : info.ancestorIdxs) {
            const Recipe* ancestorRecipe =
                pm_.findRecipe(composites_[ai].layer->name());
            if (ancestorRecipe)
                info.ancestorSeedParams = mergeRecipeParams(
                    info.ancestorSeedParams, ancestorRecipe->seed_parameters);
        }
    }

    // Engine-derived coarse occupancy (M18.5): a ROOT composite layer that has an
    // occupancy-bearing recipe but NO registered layer generator gets its coarse
    // occupancy synthesized from the recipe's own carve field, so the surface
    // lives in exactly one place and the §4 superset invariant is guaranteed, not
    // authored. A layer with a registered generator keeps it (opt-in).
    for (auto& info : composites_) {
        if (info.parentIdx >= 0) continue;  // only root layers are generator-loaded
        bool hasGen = false;
        for (const auto& g : pm_.layerGenerators())
            if (g.layer_name == info.layer->name()) { hasGen = true; break; }
        if (hasGen) continue;

        const Recipe* recipe = pm_.findRecipe(info.layer->name());
        if (!recipe || !recipe->occupancy.present) continue;

        // Resolve the occupancy at the recipe's own (root) seed level — the carve
        // field is sampled at world position, so no per-macro __altitude is needed.
        ResolvedRecipe resolved = resolveRecipe(*recipe, pm_, {});
        if (!resolved.occupancy.noise) continue;  // unresolved id (validateRecipes guards this)

        auto gen = std::make_unique<DerivedOccupancyGen>();
        gen->occ             = std::move(resolved.occupancy);
        gen->flat            = flattenParams(gen->occ.params);
        gen->macroVoxelSizeM = info.layer->voxelSizeM();
        gen->childVoxelSizeM = info.childLayer->voxelSizeM();
        gen->ratio           = info.ratio;
        gen->worldSeed       = worldSeed_;
        gen->block           = dominantInteriorMaterial(*recipe, pm_);
        info.derivedOcc      = std::move(gen);
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

        // Merge the cached ancestor seed_parameters (root → immediate parent,
        // precomputed at construction) over the reserved per-macro base. The
        // inherited set is a pure function of (world_seed, ancestor coords,
        // recipes), so it re-derives identically after a clean evict (§4/§11).
        const std::vector<RecipeParamValue> inherited = mergeRecipeParams(
            {altParam, matParam}, info.ancestorSeedParams);

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

    // Remove child chunks from this level's child layer. Only terminal-layer
    // chunks can carry player edits worth saving (the manager's own block-voxel
    // writes use setVoxelNoDirty, so a dirty composite chunk cannot occur from
    // engine activity).
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

bool DecompositionManager::chunkPinned(size_t ci, ChunkCoord cc) const {
    const CompositeLayerInfo& info = composites_[ci];

    // Own layer: O(1) counter maintained at markPending/markDecomposed time.
    auto it = info.pendingChunks.find(cc);
    if (it != info.pendingChunks.end() && it->second > 0) return true;

    // Deeper layers: the exhaustive per-macro subtree scan is only needed while
    // some finer layer actually has jobs in flight — rare during eviction, and
    // free to rule out via the per-layer pending totals.
    bool deeperPending = false;
    for (size_t cj = ci + 1; cj < composites_.size() && !deeperPending; ++cj)
        deeperPending = composites_[cj].state.pendingCount() > 0;
    if (!deeperPending) return false;

    const int n = info.layer->chunkSizeVoxels();
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (subtreePending(ci, chunkmath::chunkLocalToVoxel(cc, x, y, z, n)))
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
    // range — and the front-end remeshes it via the newlyAtomic diff. NoDirty:
    // rendered-state update, not a player edit.
    if (haveVoxel)
        info.layer->setVoxelNoDirty(
            chunkmath::voxelCenter(macro, info.layer->voxelSizeM()), restored);
}

// ── Budget enforcement ────────────────────────────────────────────────────────

void DecompositionManager::enforceLayerBudget(size_t ci, ChunkCoord camChunkComp,
                                               const WorldCoord& cameraPos,
                                               double approachRadiusM,
                                               std::vector<LayerTickDiff>& diffs) {
    CompositeLayerInfo& info = composites_[ci];
    Layer& layer = *info.layer;
    // This layer's own decompose radius (its macros decompose, and its children are
    // approach-pinned, within it); falls back to the tick-wide radius when unset.
    const double effRadius = info.decomposeRadiusM > 0.0 ? info.decomposeRadiusM
                                                         : approachRadiusM;
    const double radiusSq = effRadius * effRadius;
    const int n = layer.chunkSizeVoxels();

    const LayerDef* def = config_.findLayer(layer.name());
    if (def && def->resident_chunk_budget > 0 &&
        static_cast<int>(layer.chunks().size()) > def->resident_chunk_budget) {
        const int budget = def->resident_chunk_budget;

        // Collect evictable (clean, non-pending, out-of-approach) chunks with
        // their camera distance. Chunks within the approach radius are pinned:
        // budget-evicting them would re-decompose next tick (churn), so the cap
        // is soft inside the approach bubble and hard outside it.
        struct Candidate { ChunkCoord coord; int dist; };
        std::vector<Candidate> candidates;
        candidates.reserve(layer.chunks().size());
        for (const auto& kv : layer.chunks()) {
            const ChunkCoord& cc = kv.first;
            if (kv.second->dirty()) continue;  // pin dirty (player-edited)
            if (chunkPinned(ci, cc)) continue; // pin in-flight subtrees
            if (chunkSurfaceDistSq(cc, layer.voxelSizeM(), n, cameraPos) <= radiusSq)
                continue;                      // pin near (approach bubble)

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
            // A re-atomization can remove several sibling chunks at once, so
            // track the resident count from the store, not by hand.
            if (static_cast<int>(layer.chunks().size()) <= budget) break;
            if (!layer.getChunk(cand.coord)) continue;  // collapsed via a sibling

            if (info.parentIdx >= 0) {
                // Non-root layer: shed this chunk by collapsing its parent macro
                // (forced — budget pressure overrides the view-range check),
                // restoring the parent's block voxel instead of leaving a hole.
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

    // ── Terminal-child budget ─────────────────────────────────────────────────
    // A terminal child layer has no composites_ entry (no streaming-evict loop of
    // its own); its chunks exist only under this layer's decomposed macros and
    // are the bulk of the GPU buffer-handle load (ARCHITECTURE §10: bgfx caps
    // static buffers at 4096). When the child's cap is exceeded, collapse the
    // farthest owning macros — forced re-atomization restores their block voxels,
    // so the cap shrinks the fine bubble without leaving holes.
    Layer& child = *info.childLayer;
    if (child.mode() != VoxelMode::terminal) return;
    const LayerDef* childDef = config_.findLayer(child.name());
    if (!childDef || childDef->resident_chunk_budget <= 0) return;
    const int childBudget = childDef->resident_chunk_budget;
    if (static_cast<int>(child.chunks().size()) <= childBudget) return;

    const ChunkCoord childCam = chunkmath::worldToChunk(
        cameraPos, child.voxelSizeM(), child.chunkSizeVoxels());
    struct ChildCand { ChunkCoord coord; chunkmath::VoxelCoord macro; int dist; };
    std::vector<ChildCand> childCands;
    childCands.reserve(child.chunks().size());
    for (const auto& kv : child.chunks()) {
        const ChunkCoord& cc = kv.first;
        if (kv.second->dirty()) continue;  // pin player-edited chunks
        const chunkmath::VoxelCoord owner = chunkmath::childToParentVoxel(
            chunkmath::chunkLocalToVoxel(cc, 0, 0, 0, child.chunkSizeVoxels()),
            info.ratio);
        if (!info.state.isDecomposed(owner)) continue;  // orphan (caller-owned)
        // Pin owners inside the approach bubble (would re-decompose: churn).
        if (macroSurfaceDistSq(owner, layer.voxelSizeM(), cameraPos) <= radiusSq)
            continue;
        int dist = std::max({std::abs(cc.x - childCam.x),
                             std::abs(cc.y - childCam.y),
                             std::abs(cc.z - childCam.z)});
        childCands.push_back({cc, owner, dist});
    }
    std::sort(childCands.begin(), childCands.end(),
              [](const ChildCand& a, const ChildCand& b){ return a.dist > b.dist; });

    for (const ChildCand& cand : childCands) {
        if (static_cast<int>(child.chunks().size()) <= childBudget) break;
        if (!child.getChunk(cand.coord)) continue;  // removed with a sibling
        if (info.state.isDecomposed(cand.macro))
            reatomize(ci, cand.macro, cameraPos, /*force=*/true, diffs);
    }
}

// ── Immutable-layer streaming (M16, L5) ────────────────────────────────────────

void DecompositionManager::streamImmutableLayers(const WorldCoord& cameraPos,
                                                 int loadPerFrame,
                                                 std::vector<LayerTickDiff>& diffs) {
    VOXEL_PROFILE_SCOPE("decomp.immutable");
    for (ImmutableLayerInfo& info : immutables_) {
        Layer& layer = *info.layer;
        const ChunkCoord camChunk = chunkmath::worldToChunk(
            cameraPos, layer.voxelSizeM(), layer.chunkSizeVoxels());

        diffs.emplace_back();
        LayerTickDiff& diff = diffs.back();
        diff.compositeLayerName = layer.name();
        diff.isImmutable        = true;

        // Load: every chunk the layer's StreamingVolume wants that is not resident,
        // up to the per-frame cap. Immutable chunks regenerate deterministically
        // from seed, so a chunk that was evicted and re-entered rebuilds identically.
        int loaded = 0;
        for (const ChunkCoord& cc : lod_.desiredChunks(camChunk, layer.name())) {
            if (loaded >= loadPerFrame) break;
            if (layer.getChunk(cc)) continue;
            if (Chunk* chunk = layer.loadChunk(cc, info.genFn, info.genUD)) {
                fireChunkCreated(layer, *chunk);
                diff.newCompChunks.push_back(cc);
                ++loaded;
            }
        }

        // Evict: chunks now outside the volume (load radius + hysteresis). No
        // dirty/persist path — immutable chunks are never player-edited and
        // regenerate from seed, so they are simply dropped (ARCHITECTURE §9).
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : layer.chunks())
            if (lod_.shouldEvict(camChunk, kv.first, layer.name()))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& cc : toEvict) {
            fireChunkEvicted(layer, cc);
            diff.evictedCompChunks.push_back(cc);
            layer.unloadChunk(cc);
        }

        // Budget: if the resident set still exceeds the per-layer cap, drop the
        // farthest chunks first (same policy as composite/terminal layers). No
        // re-atomization — immutable chunks have no parent macro to restore.
        const LayerDef* def = config_.findLayer(layer.name());
        if (!def || def->resident_chunk_budget <= 0) continue;
        const int budget = def->resident_chunk_budget;
        if (static_cast<int>(layer.chunks().size()) <= budget) continue;

        struct Candidate { ChunkCoord coord; int dist; };
        std::vector<Candidate> candidates;
        candidates.reserve(layer.chunks().size());
        for (const auto& kv : layer.chunks()) {
            const ChunkCoord& cc = kv.first;
            int dist = std::max({std::abs(cc.x - camChunk.x),
                                 std::abs(cc.y - camChunk.y),
                                 std::abs(cc.z - camChunk.z)});
            candidates.push_back({cc, dist});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.dist > b.dist; });
        for (const Candidate& cand : candidates) {
            if (static_cast<int>(layer.chunks().size()) <= budget) break;
            if (!layer.getChunk(cand.coord)) continue;
            fireChunkEvicted(layer, cand.coord);
            diff.evictedCompChunks.push_back(cand.coord);
            layer.unloadChunk(cand.coord);
        }
    }
}

// ── Main tick ─────────────────────────────────────────────────────────────────

std::vector<LayerTickDiff> DecompositionManager::tick(const WorldCoord& cameraPos,
                                                       double approachRadiusM,
                                                       int loadPerFrame,
                                                       int decompPerFrame,
                                                       int applyPerFrame) {
    VOXEL_PROFILE_SCOPE("decomp.tick");
    // One diff per composite layer, indexed in composites_ order.
    std::vector<LayerTickDiff> diffs(composites_.size());
    for (size_t i = 0; i < composites_.size(); ++i) {
        diffs[i].compositeLayerName = composites_[i].layer->name();
        diffs[i].childLayerName     = composites_[i].childLayer->name();
    }

    // ── 1. Apply completed decomposition jobs (budgeted) ─────────────────────
    // Completed results queue in backlog_ and at most applyPerFrame are applied
    // per tick (non-positive = unlimited). Each application inserts child chunks
    // and obliges the caller to build their meshes the same frame, so an
    // unbounded drain turns a burst of completions into a single-frame hitch.
    // Unapplied results still count as inFlight() and their macros stay pending,
    // so eviction pinning keeps their target chunks resident.
    {
        auto fresh = worker_.drain();
        backlog_.insert(backlog_.end(),
                        std::make_move_iterator(fresh.begin()),
                        std::make_move_iterator(fresh.end()));
    }
    int applied = 0;
    while (!backlog_.empty() && (applyPerFrame <= 0 || applied < applyPerFrame)) {
        DecompositionResult result = std::move(backlog_.front());
        backlog_.pop_front();
        ++applied;

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

        // markDecomposed clears the macro's pending state; mirror that in the
        // per-chunk pending counter used for O(1) eviction pinning.
        {
            const ChunkCoord owning = chunkmath::voxelToChunkLocal(
                result.macro, info.layer->chunkSizeVoxels()).chunk;
            auto pit = info.pendingChunks.find(owning);
            if (pit != info.pendingChunks.end() && --pit->second <= 0)
                info.pendingChunks.erase(pit);
        }
        info.state.markDecomposed(result.macro);
        diff.newlyDecomposed.push_back(result.macro);

        // Clear the atomic block voxel in the composite layer so the composite
        // chunk mesh no longer shows it (the child voxels render instead), caching
        // it first so bottom-up re-atomization can restore the block. NoDirty:
        // this is a rendered-state update, not a player edit — it must not pin
        // the chunk against budget eviction or be persisted.
        const WorldCoord macroCenter =
            chunkmath::voxelCenter(result.macro, info.layer->voxelSizeM());
        info.originalVoxel[result.macro] = info.layer->getVoxel(macroCenter);
        info.layer->setVoxelNoDirty(macroCenter, Voxel::empty());
    }

    // ── 2. Stream composite-layer chunks (load + evict) ──────────────────────
    //    Processed coarsest-first. A finer composite layer only loads chunks
    //    within its LOD budget; eviction is done after loading so the newly
    //    resident chunks are not immediately evicted.
    for (size_t ci = 0; ci < composites_.size(); ++ci) {
        VOXEL_PROFILE_SCOPE("decomp.stream");  // per-layer load + evict + budget
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
            // No registered generator but the recipe carries occupancy: synthesize
            // the coarse occupancy from the carve field (M18.5).
            if (!genFn && info.derivedOcc) {
                genFn = &DecompositionManager::derivedOccupancyGenerator;
                genUD = info.derivedOcc.get();
            }

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
            if (chunkPinned(ci, cc)) continue;  // jobs in flight at some depth

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
                    // Pin while the parent macro is still inside ITS layer's
                    // decompose radius: collapsing it would just re-trigger
                    // decomposition next tick (decompose/collapse thrash when a
                    // child layer's eviction radius is smaller than the radius that
                    // decomposed the parent).
                    const double parentRadius = parent.decomposeRadiusM > 0.0
                                              ? parent.decomposeRadiusM : approachRadiusM;
                    if (macroSurfaceDistSq(parentMacro, parent.layer->voxelSizeM(),
                                           cameraPos) > parentRadius * parentRadius) {
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
        enforceLayerBudget(ci, camChunk, cameraPos, approachRadiusM, diffs);
    }

    // ── 3. Approach-trigger: enqueue undecomposed macro voxels near camera ───
    //    Candidates are gathered from RESIDENT chunks only (a macro in an
    //    unloaded chunk reads empty and can never trigger), with whole chunks
    //    rejected by AABB distance before their voxels are scanned and the
    //    per-voxel gates ordered cheapest-first. Eligible macros are then
    //    enqueued NEAREST-FIRST so the block in front of the player never waits
    //    behind peripheral work. A fine layer's macro is only eligible once its
    //    parent macro is decomposed (the chain requirement).
    if (decompPerFrame > 0) {
        VOXEL_PROFILE_SCOPE("decomp.approach");  // per-voxel candidate scan + enqueue
        struct Candidate { double distSq; size_t ci; chunkmath::VoxelCoord macro; };
        std::vector<Candidate> candidates;

        for (size_t ci = 0; ci < composites_.size(); ++ci) {
            CompositeLayerInfo& info = composites_[ci];
            Layer& layer = *info.layer;
            const double voxelSize = layer.voxelSizeM();
            const int    n         = layer.chunkSizeVoxels();

            // Each composite layer triggers on its OWN decompose radius (so a coarse
            // layer can reveal its child far out while a finer layer only refines up
            // close); falls back to the tick-wide radius when the layer omits one.
            const double effRadius = info.decomposeRadiusM > 0.0 ? info.decomposeRadiusM
                                                                 : approachRadiusM;
            if (effRadius <= 0.0) continue;
            const double radiusSq = effRadius * effRadius;

            for (const auto& kv : layer.chunks()) {
                if (chunkSurfaceDistSq(kv.first, voxelSize, n, cameraPos) > radiusSq)
                    continue;

                const Chunk& chunk = *kv.second;
                for (int z = 0; z < n; ++z)
                    for (int y = 0; y < n; ++y)
                        for (int x = 0; x < n; ++x) {
                            // Gate order, cheapest first: solid (direct array
                            // read — a cleared voxel was already decomposed),
                            // AABB distance, then the state hash lookups.
                            if (chunk.at(x, y, z).isEmpty()) continue;
                            const chunkmath::VoxelCoord macro =
                                chunkmath::chunkLocalToVoxel(kv.first, x, y, z, n);
                            const double distSq =
                                macroSurfaceDistSq(macro, voxelSize, cameraPos);
                            if (distSq > radiusSq) continue;
                            if (!info.state.needsDecompose(macro)) continue;
                            if (info.parentIdx >= 0) {
                                const CompositeLayerInfo& parent =
                                    composites_[info.parentIdx];
                                if (!parent.state.isDecomposed(
                                        chunkmath::childToParentVoxel(macro, parent.ratio)))
                                    continue;
                            }
                            candidates.push_back({distSq, ci, macro});
                        }
            }
        }

        // Nearest-first; ties broken by (layer, coords) so the enqueue order is
        // fully deterministic despite unordered chunk-store iteration.
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.distSq != b.distSq) return a.distSq < b.distSq;
                      if (a.ci != b.ci) return a.ci < b.ci;
                      if (a.macro.x != b.macro.x) return a.macro.x < b.macro.x;
                      if (a.macro.y != b.macro.y) return a.macro.y < b.macro.y;
                      return a.macro.z < b.macro.z;
                  });

        int enqueued = 0;
        for (const Candidate& c : candidates) {
            if (enqueued >= decompPerFrame) break;
            CompositeLayerInfo& info = composites_[c.ci];
            if (info.state.markPending(c.macro)) {
                ++info.pendingChunks[chunkmath::voxelToChunkLocal(
                    c.macro, info.layer->chunkSizeVoxels()).chunk];
                worker_.enqueue(makeJob(info, c.macro));
                ++enqueued;
            }
        }
    }

    // ── 4. Stream immutable layers (M16, L5) ─────────────────────────────────
    //    Each immutable layer's meshes stream in/out under its own StreamingVolume
    //    + resident_chunk_budget, exactly like composite/terminal layers, instead
    //    of being generated-once-and-fully-resident. Appends one isImmutable diff
    //    per layer; immutable chunks skip dirty/persist.
    streamImmutableLayers(cameraPos, loadPerFrame, diffs);

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

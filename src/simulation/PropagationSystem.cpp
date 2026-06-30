#include "simulation/PropagationSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>

#include "core/EngineConfig.h"
#include "core/Tuning.h"
#include "world/Layer.h"
#include "world/World.h"

namespace sim {

using chunkmath::VoxelCoord;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Cost to *enter* a macro of aggregate structural_strength s: 1 / maxSpan(s),
// where maxSpan(s) = clamp(s·kSupportSpanPerStrength, 0, kMaxSupportSpan) is the
// unsupported span (in macro-voxels) the material can bridge. Strength below
// kMinSupportStrength transmits no support at all → infinite cost (§7).
double enterCost(double strength) {
    namespace tp = tuning::physics;
    if (strength < tp::kMinSupportStrength) return kInf;
    double span = strength * tp::kSupportSpanPerStrength;
    span = std::clamp(span, 0.0, static_cast<double>(tp::kMaxSupportSpan));
    if (span <= 0.0) return kInf;
    return 1.0 / span;
}

}  // namespace

PropagationSystem::PropagationSystem(const World& world) : world_(world) {
    // The terminal layer is the player-editable layer the edit path forwards to.
    child_ = world_.primaryLayer();

    // Build the composite chain bottom-up: level 0 is the layer whose decompose_to
    // is the terminal layer; each coarser ancestor (the layer whose decompose_to
    // is the prior level's composite) is the next level. This is the same
    // ancestor chain the M10 cascade walks — here reused for upward re-aggregation.
    if (child_) {
        std::string childName = child_->name();
        const Layer* childLayer = child_;
        while (true) {
            const Layer* composite = nullptr;
            for (const auto& l : world_.layers()) {
                if (l->decomposeTo() && *l->decomposeTo() == childName) {
                    composite = l.get();
                    break;
                }
            }
            if (!composite) break;
            Level lvl;
            lvl.composite = composite;
            lvl.child     = childLayer;
            lvl.ratio     = chunkmath::layerRatio(composite->voxelSizeM(),
                                                  childLayer->voxelSizeM());
            levels_.push_back(std::move(lvl));
            childName  = composite->name();
            childLayer = composite;
        }
    }

    // Record immutable layers as anchor sources (propagation stops dead at an
    // immutable boundary at every level).
    for (const auto& l : world_.layers())
        if (l->mode() == VoxelMode::immutable)
            immutableLayers_.push_back(l.get());
}

const Layer* PropagationSystem::compositeLayer(int level) const {
    return (level >= 0 && level < levelCount()) ? levels_[level].composite : nullptr;
}

const Layer* PropagationSystem::childLayerAt(int level) const {
    return (level >= 0 && level < levelCount()) ? levels_[level].child : nullptr;
}

double PropagationSystem::macroVoxelSizeM(int level) const {
    const Layer* c = compositeLayer(level);
    return c ? c->voxelSizeM() : 0.0;
}

double PropagationSystem::childVoxelSizeM(int level) const {
    const Layer* c = childLayerAt(level);
    return c ? c->voxelSizeM() : 0.0;
}

WorldCoord PropagationSystem::macroCenter(int level, VoxelCoord macro) const {
    return chunkmath::voxelCenter(macro, macroVoxelSizeM(level));
}

VoxelCoord PropagationSystem::parentMacro(const WorldCoord& pos) const {
    const VoxelCoord childV = chunkmath::worldToVoxel(pos, child_->voxelSizeM());
    return chunkmath::childToParentVoxel(childV, levels_[0].ratio);
}

void PropagationSystem::onVoxelModified(const WorldCoord& pos,
                                        const Voxel& oldVoxel,
                                        const Voxel& newVoxel) {
    if (!active()) return;
    const VoxelCoord macro = parentMacro(pos);

    // O(1) incremental update of the level-0 running sum: apply the old→new
    // strength delta *only* when a baseline already exists. When it does not, the
    // delta is dropped — re-summing children here is forbidden (§7 "Performance");
    // the driver's bounded recomputeAggregate establishes the baseline from
    // current world state instead, and that state already reflects this edit. The
    // upward cascade to coarser levels is driven by recomputeAggregate, not here.
    auto& agg = levels_[0].aggregates;
    auto it = agg.find(macro);
    if (it != agg.end()) {
        it->second += static_cast<double>(newVoxel.material.structural_strength) -
                      static_cast<double>(oldVoxel.material.structural_strength);
    }
    levels_[0].dirty.insert(macro);
}

bool PropagationSystem::hasDirty() const {
    for (const Level& l : levels_)
        if (!l.dirty.empty()) return true;
    return false;
}

std::vector<VoxelCoord> PropagationSystem::drainDirty(int level) {
    if (level < 0 || level >= levelCount()) return {};
    auto& d = levels_[level].dirty;
    std::vector<VoxelCoord> out(d.begin(), d.end());  // already sorted
    d.clear();
    return out;
}

PropagationSystem::MacroInfo PropagationSystem::classify(int level,
                                                         VoxelCoord macro) const {
    MacroInfo mi;
    const Layer* comp = levels_[level].composite;
    const WorldCoord c = macroCenter(level, macro);
    const ChunkCoord cc = chunkmath::worldToChunk(c, comp->voxelSizeM(),
                                                  comp->chunkSizeVoxels());
    if (!comp->getChunk(cc)) return mi;  // non-resident
    mi.resident = true;
    const Voxel v = comp->getVoxel(c);
    if (!v.isEmpty()) {  // resident, undecomposed → atomic block
        mi.atomic = true;
        mi.blockStrength = v.material.structural_strength;
    }
    return mi;
}

double PropagationSystem::childEffectiveStrength(int level, VoxelCoord childVoxel,
                                                 Memo& memo) const {
    if (level == 0) {
        // Terminal voxels carry their material strength directly.
        const double cvs = child_->voxelSizeM();
        return child_->getVoxel(chunkmath::voxelCenter(childVoxel, cvs))
            .material.structural_strength;
    }
    // A coarser level's child is the next-finer composite macro; its effective
    // strength is its own aggregate (recursively → terminal at level 0).
    return macroAggregate(level - 1, childVoxel, memo);
}

double PropagationSystem::childStrengthSum(int level, VoxelCoord macro,
                                           Memo& memo) const {
    const int64_t ratio = levels_[level].ratio;
    const VoxelCoord cmin = chunkmath::childVoxelMin(macro, ratio);
    double sum = 0.0;
    for (int64_t dz = 0; dz < ratio; ++dz)
        for (int64_t dy = 0; dy < ratio; ++dy)
            for (int64_t dx = 0; dx < ratio; ++dx)
                sum += childEffectiveStrength(
                    level, {cmin.x + dx, cmin.y + dy, cmin.z + dz}, memo);
    return sum;
}

double PropagationSystem::cachedSum(int level, VoxelCoord macro,
                                    Memo& memo) const {
    const auto& agg = levels_[level].aggregates;
    auto it = agg.find(macro);
    if (it != agg.end()) return it->second;
    AggMemo& m = memo.level[level];
    auto mit = m.find(macro);
    if (mit != m.end()) return mit->second;
    const double s = childStrengthSum(level, macro, memo);
    m.emplace(macro, s);
    return s;
}

bool PropagationSystem::macroResident(int level, VoxelCoord macro) const {
    return classify(level, macro).resident;
}

double PropagationSystem::macroAggregate(int level, VoxelCoord macro,
                                         Memo& memo) const {
    const MacroInfo mi = classify(level, macro);
    if (!mi.resident) return 0.0;
    if (mi.atomic) return mi.blockStrength;
    const int64_t r = levels_[level].ratio;
    return cachedSum(level, macro, memo) / static_cast<double>(r * r * r);
}

bool PropagationSystem::macroSolid(int level, VoxelCoord macro, Memo& memo) const {
    const MacroInfo mi = classify(level, macro);
    if (!mi.resident) return false;
    if (mi.atomic) return true;
    return cachedSum(level, macro, memo) > 0.0;
}

bool PropagationSystem::isAnchor(int level, VoxelCoord macro) const {
    // Conservative boundary rule: a non-resident neighbor counts as solid support
    // ("unknown ⇒ supported"), so the streaming edge never spuriously collapses.
    if (!macroResident(level, macro)) return true;
    // Immutable-layer voxels are infinite-effective anchors — propagation stops
    // dead at an immutable boundary (e.g. bedrock under a structure).
    const WorldCoord c = macroCenter(level, macro);
    for (const Layer* im : immutableLayers_)
        if (!im->getVoxel(c).isEmpty()) return true;
    return false;
}

float PropagationSystem::aggregateStrength(int level, VoxelCoord macro) const {
    if (level < 0 || level >= levelCount()) return 0.0f;
    Memo memo = makeMemo();
    return static_cast<float>(macroAggregate(level, macro, memo));
}

void PropagationSystem::recomputeAggregate(int level, VoxelCoord macro) {
    if (!active() || level < 0 || level >= levelCount()) return;
    const MacroInfo mi = classify(level, macro);
    // Only a decomposed macro maintains a running child sum; an atomic macro reads
    // its strength straight from its own block material, so it needs no baseline.
    if (mi.resident && !mi.atomic) {
        Memo memo = makeMemo();
        levels_[level].aggregates[macro] = childStrengthSum(level, macro, memo);
    } else {
        levels_[level].aggregates.erase(macro);  // no longer a decomposed macro
    }
    // Upward cascade: this macro is a child cell of exactly one parent macro at the
    // next-coarser level, whose aggregate now depends on the value we just wrote —
    // mark it dirty so the driver re-aggregates and re-floods it. The chain stops
    // at the coarsest (root) composite level.
    if (level + 1 < levelCount()) {
        const VoxelCoord parent =
            chunkmath::childToParentVoxel(macro, levels_[level + 1].ratio);
        levels_[level + 1].dirty.insert(parent);
    }
}

std::vector<PropagationSystem::Unstable> PropagationSystem::findUnstable(
    int level, const std::vector<VoxelCoord>& candidates) const {
    std::vector<Unstable> result;
    if (!active() || level < 0 || level >= levelCount()) return result;

    Memo memo = makeMemo();  // per-call aggregate cache, one map per level

    // ── Candidate set: dirty macros that are still solid, plus the solid
    //    6-neighbors of every dirty macro (the neighbor cascade — a removed
    //    support re-evaluates its neighbors at THIS level). The upward cascade to
    //    coarser levels is driven separately, by recomputeAggregate marking each
    //    macro's parent dirty (gap audit G1).
    std::set<VoxelCoord, VoxelCoordLess> cand;
    for (const VoxelCoord& d : candidates) {
        if (macroSolid(level, d, memo)) cand.insert(d);
        for (const VoxelCoord& n : neighbors6(d))
            if (macroSolid(level, n, memo)) cand.insert(n);
    }
    if (cand.empty()) return result;

    // ── Region discovery: the connected solid macros reachable from the
    //    candidates, expanded breadth-first (in graph-distance order) and capped at
    //    kMaxSupportFloodNodes. Nearest-first matters when the cap is hit: the
    //    region is then the kMaxNodes macros NEAREST the candidates, so an anchor a
    //    few macros away (e.g. the bedrock / non-resident floor below a surface
    //    edit) always lands inside it. A coord-order flood would instead race off
    //    toward the min-coordinate corner and could cap out before ever reaching a
    //    nearby anchor, spuriously collapsing a supported structure. Each ring is
    //    drained in sorted-coord order so the truncated set stays reproducible, and
    //    when the whole connected region fits under the cap the discovered set is
    //    identical to the old order — only the truncation behaviour changes.
    const int kMaxNodes = engineConfig().physicsMaxSupportFloodNodes;
    std::set<VoxelCoord, VoxelCoordLess> region;
    std::set<VoxelCoord, VoxelCoordLess> frontier(cand.begin(), cand.end());
    while (!frontier.empty() && static_cast<int>(region.size()) < kMaxNodes) {
        std::set<VoxelCoord, VoxelCoordLess> next;
        for (const VoxelCoord& m : frontier) {  // this ring, sorted-coord order
            if (static_cast<int>(region.size()) >= kMaxNodes) break;
            if (!region.insert(m).second) continue;
            for (const VoxelCoord& n : neighbors6(m))
                if (!region.count(n) && macroSolid(level, n, memo)) next.insert(n);
        }
        frontier.swap(next);
    }

    // ── Multi-source Dijkstra over the region. Sources are anchor-adjacent solid
    //    macros, seeded with their own entering cost (the anchor emits potential
    //    1.0, drained by 1/maxSpan on entry). dist[m] is the minimum total drain
    //    from any anchor; potential = kAnchorPotential − dist, unstable iff ≤ 0.
    const double kAnchor = tuning::physics::kAnchorPotential;
    std::unordered_map<VoxelCoord, double, chunkmath::VoxelCoordHash> dist;

    struct QItem { double d; VoxelCoord c; };
    struct QCmp {  // min-heap by dist, tie-broken by coord for determinism
        bool operator()(const QItem& a, const QItem& b) const {
            if (a.d != b.d) return a.d > b.d;
            return VoxelCoordLess{}(b.c, a.c);
        }
    };
    std::priority_queue<QItem, std::vector<QItem>, QCmp> pq;

    for (const VoxelCoord& m : region) {
        bool anchored = false;
        for (const VoxelCoord& n : neighbors6(m))
            if (isAnchor(level, n)) { anchored = true; break; }
        if (!anchored) continue;
        const double d = enterCost(macroAggregate(level, m, memo));
        auto it = dist.find(m);
        if (it == dist.end() || d < it->second) {
            dist[m] = d;
            pq.push({d, m});
        }
    }

    while (!pq.empty()) {
        const QItem cur = pq.top();
        pq.pop();
        auto it = dist.find(cur.c);
        if (it == dist.end() || cur.d > it->second) continue;  // stale
        // An exhausted macro (potential ≤ 0) transmits no support onward, so it
        // is never relaxed further — this also bounds the flood to ~kMaxSupportSpan.
        if (cur.d >= kAnchor) continue;
        for (const VoxelCoord& n : neighbors6(cur.c)) {
            if (!region.count(n)) continue;
            const double nd = cur.d + enterCost(macroAggregate(level, n, memo));
            auto nit = dist.find(n);
            if (nit == dist.end() || nd < nit->second) {
                dist[n] = nd;
                pq.push({nd, n});
            }
        }
    }

    // ── Collect the unstable candidates (residual potential ≤ 0) in sorted order.
    for (const VoxelCoord& c : cand) {
        auto it = dist.find(c);
        const double d = (it == dist.end()) ? kInf : it->second;
        if (d < kAnchor) continue;  // supported
        Unstable u;
        u.macro = c;
        u.aggregate_strength = static_cast<float>(macroAggregate(level, c, memo));
        u.support_potential =
            std::isinf(d) ? -static_cast<float>(kAnchor)
                          : static_cast<float>(kAnchor - d);
        result.push_back(u);
    }
    return result;
}

}  // namespace sim

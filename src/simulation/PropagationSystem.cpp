#include "simulation/PropagationSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>

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

// The 6 axis-neighbors of a macro voxel — the shared NeighborWalk.h walk,
// reused at macro-voxel granularity here and at terminal-voxel granularity by
// the M14 field passes.
std::array<VoxelCoord, 6> neighbors(VoxelCoord m) { return neighbors6(m); }

}  // namespace

PropagationSystem::PropagationSystem(const World& world) : world_(world) {
    // The child layer is the terminal (player-editable) layer the edit path
    // forwards to; the composite layer is the one decomposing into it (the single
    // M13 level). Record immutable layers as anchor sources.
    child_ = world_.primaryLayer();
    if (child_) {
        for (const auto& l : world_.layers()) {
            if (l->decomposeTo() && *l->decomposeTo() == child_->name()) {
                composite_ = l.get();
                break;
            }
        }
    }
    for (const auto& l : world_.layers())
        if (l->mode() == VoxelMode::immutable)
            immutableLayers_.push_back(l.get());

    if (active())
        ratio_ = chunkmath::layerRatio(composite_->voxelSizeM(),
                                       child_->voxelSizeM());
}

double PropagationSystem::macroVoxelSizeM() const {
    return composite_ ? composite_->voxelSizeM() : 0.0;
}

WorldCoord PropagationSystem::macroCenter(VoxelCoord macro) const {
    return chunkmath::voxelCenter(macro, macroVoxelSizeM());
}

VoxelCoord PropagationSystem::parentMacro(const WorldCoord& pos) const {
    const VoxelCoord childV = chunkmath::worldToVoxel(pos, child_->voxelSizeM());
    return chunkmath::childToParentVoxel(childV, ratio_);
}

void PropagationSystem::onVoxelModified(const WorldCoord& pos,
                                        const Voxel& oldVoxel,
                                        const Voxel& newVoxel) {
    if (!active()) return;
    const VoxelCoord macro = parentMacro(pos);

    // O(1) incremental update: apply the old→new strength delta to the running
    // sum *only* when a baseline already exists. When it does not, the delta is
    // dropped — re-summing children here is forbidden (§7 "Performance"); the
    // driver's bounded recomputeAggregate establishes the baseline from current
    // world state instead, and that state already reflects this edit.
    auto it = aggregates_.find(macro);
    if (it != aggregates_.end()) {
        it->second += static_cast<double>(newVoxel.material.structural_strength) -
                      static_cast<double>(oldVoxel.material.structural_strength);
    }
    dirty_.insert(macro);
}

std::vector<VoxelCoord> PropagationSystem::drainDirty() {
    std::vector<VoxelCoord> out(dirty_.begin(), dirty_.end());  // already sorted
    dirty_.clear();
    return out;
}

PropagationSystem::MacroInfo PropagationSystem::classify(VoxelCoord macro) const {
    MacroInfo mi;
    if (!active()) return mi;
    const WorldCoord c = macroCenter(macro);
    const ChunkCoord cc = chunkmath::worldToChunk(c, composite_->voxelSizeM(),
                                                  composite_->chunkSizeVoxels());
    if (!composite_->getChunk(cc)) return mi;  // non-resident
    mi.resident = true;
    const Voxel v = composite_->getVoxel(c);
    if (!v.isEmpty()) {  // resident, undecomposed → atomic block
        mi.atomic = true;
        mi.blockStrength = v.material.structural_strength;
    }
    return mi;
}

double PropagationSystem::childStrengthSum(VoxelCoord macro) const {
    const VoxelCoord cmin = chunkmath::childVoxelMin(macro, ratio_);
    const double cvs = child_->voxelSizeM();
    double sum = 0.0;
    for (int64_t dz = 0; dz < ratio_; ++dz)
        for (int64_t dy = 0; dy < ratio_; ++dy)
            for (int64_t dx = 0; dx < ratio_; ++dx) {
                const VoxelCoord cv{cmin.x + dx, cmin.y + dy, cmin.z + dz};
                sum += child_->getVoxel(chunkmath::voxelCenter(cv, cvs))
                           .material.structural_strength;
            }
    return sum;
}

double PropagationSystem::cachedSum(VoxelCoord macro, AggMemo& memo) const {
    auto it = aggregates_.find(macro);
    if (it != aggregates_.end()) return it->second;
    auto mit = memo.find(macro);
    if (mit != memo.end()) return mit->second;
    const double s = childStrengthSum(macro);
    memo.emplace(macro, s);
    return s;
}

bool PropagationSystem::macroResident(VoxelCoord macro) const {
    return classify(macro).resident;
}

double PropagationSystem::macroAggregate(VoxelCoord macro, AggMemo& memo) const {
    const MacroInfo mi = classify(macro);
    if (!mi.resident) return 0.0;
    if (mi.atomic) return mi.blockStrength;
    return cachedSum(macro, memo) / static_cast<double>(ratio_ * ratio_ * ratio_);
}

bool PropagationSystem::macroSolid(VoxelCoord macro, AggMemo& memo) const {
    const MacroInfo mi = classify(macro);
    if (!mi.resident) return false;
    if (mi.atomic) return true;
    return cachedSum(macro, memo) > 0.0;
}

bool PropagationSystem::isAnchor(VoxelCoord macro) const {
    // Conservative boundary rule: a non-resident neighbor counts as solid support
    // ("unknown ⇒ supported"), so the streaming edge never spuriously collapses.
    if (!macroResident(macro)) return true;
    // Immutable-layer voxels are infinite-effective anchors — propagation stops
    // dead at an immutable boundary (e.g. bedrock under a structure).
    const WorldCoord c = macroCenter(macro);
    for (const Layer* im : immutableLayers_)
        if (!im->getVoxel(c).isEmpty()) return true;
    return false;
}

float PropagationSystem::aggregateStrength(VoxelCoord macro) const {
    AggMemo memo;
    return static_cast<float>(macroAggregate(macro, memo));
}

void PropagationSystem::recomputeAggregate(VoxelCoord macro) {
    if (!active()) return;
    const MacroInfo mi = classify(macro);
    // Only a decomposed macro maintains a running child sum; an atomic macro reads
    // its strength straight from its own block material, so it needs no baseline.
    if (mi.resident && !mi.atomic)
        aggregates_[macro] = childStrengthSum(macro);
    else
        aggregates_.erase(macro);  // no longer a decomposed macro
}

std::vector<PropagationSystem::Unstable> PropagationSystem::findUnstable(
    const std::vector<VoxelCoord>& candidates) const {
    std::vector<Unstable> result;
    if (!active()) return result;

    AggMemo memo;  // per-call aggregate cache so each macro is summed at most once

    // ── Candidate set: dirty macros that are still solid, plus the solid
    //    6-neighbors of every dirty macro (the single-level neighbor cascade —
    //    a removed support re-evaluates its neighbors at THIS level only).
    //
    //    M16 TODO: this stops at one composite level. Re-aggregating the macro's
    //    grandparents/root up the chain (reusing the M10 cascade infrastructure)
    //    is deferred to M16 (Polish and Release) — see ARCHITECTURE §7.
    std::set<VoxelCoord, VoxelCoordLess> cand;
    for (const VoxelCoord& d : candidates) {
        if (macroSolid(d, memo)) cand.insert(d);
        for (const VoxelCoord& n : neighbors(d))
            if (macroSolid(n, memo)) cand.insert(n);
    }
    if (cand.empty()) return result;

    // ── Region discovery: the connected solid macros reachable from the
    //    candidates, expanded in sorted-coord order and capped at
    //    kMaxSupportFloodNodes so the truncated set (if ever hit) is reproducible.
    const int kMaxNodes = tuning::physics::kMaxSupportFloodNodes;
    std::set<VoxelCoord, VoxelCoordLess> region;
    std::set<VoxelCoord, VoxelCoordLess> open(cand.begin(), cand.end());
    while (!open.empty() && static_cast<int>(region.size()) < kMaxNodes) {
        const VoxelCoord m = *open.begin();
        open.erase(open.begin());
        if (!region.insert(m).second) continue;
        for (const VoxelCoord& n : neighbors(m))
            if (!region.count(n) && macroSolid(n, memo)) open.insert(n);
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
        for (const VoxelCoord& n : neighbors(m))
            if (isAnchor(n)) { anchored = true; break; }
        if (!anchored) continue;
        const double d = enterCost(macroAggregate(m, memo));
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
        for (const VoxelCoord& n : neighbors(cur.c)) {
            if (!region.count(n)) continue;
            const double nd = cur.d + enterCost(macroAggregate(n, memo));
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
        u.aggregate_strength = static_cast<float>(macroAggregate(c, memo));
        u.support_potential =
            std::isinf(d) ? -static_cast<float>(kAnchor)
                          : static_cast<float>(kAnchor - d);
        result.push_back(u);
    }
    return result;
}

}  // namespace sim

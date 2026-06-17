#pragma once

#include <set>
#include <unordered_map>
#include <vector>

#include "simulation/NeighborWalk.h"  // sim::VoxelCoordLess, the shared sorted-coord order
#include "world/ChunkCoordMath.h"     // chunkmath::VoxelCoord, VoxelCoordHash
#include "world/Voxel.h"

class World;
class Layer;

// ---------------------------------------------------------------------------
// PropagationSystem — the DETECTION half of M13 upward damage propagation
// (docs/ARCHITECTURE.md §7). It decides *what is unstable* and writes nothing:
// every method reads World by const reference. The companion PhysicsSystem is
// the per-frame driver that calls into this system and fires on_structural_event
// within the tuning::physics budgets; the mandatory structural-response plugin
// owns every voxel write.
//
// Two responsibilities:
//
//   1. Incremental aggregation. Each decomposed composite macro voxel carries an
//      aggregate structural_strength = the volume-weighted average of its child
//      voxels' strength. Because all children of one macro share a voxel size the
//      volume weight is uniform, so the aggregate is simply Σ(child strength) /
//      ratio³ over the macro's full child-cell count — mining a child out lowers
//      the average, which is the whole point (§7 "Purpose"). The running Σ is
//      maintained O(1) per edit from the on_voxel_modified old→new delta
//      (onVoxelModified), called at the NetworkManager edit choke point. A full
//      re-sum (recomputeAggregate) is the bounded fallback only; recomputing
//      structure on the modification path is forbidden (§7 "Performance").
//
//   2. Support-potential flood. An anchor emits potential kAnchorPotential (1.0);
//      entering a solid macro of aggregate strength s drains it by 1/maxSpan(s),
//      where maxSpan(s) = clamp(s·kSupportSpanPerStrength, 0, kMaxSupportSpan);
//      strength < kMinSupportStrength transmits nothing. A macro is unstable iff
//      its residual potential ≤ 0. Anchors are (a) immutable-layer voxels and
//      (b) the boundary of the resident region — a non-resident neighbor counts
//      as solid support ("unknown ⇒ supported", the conservative rule that stops
//      streaming edges and immutable boundaries from spuriously collapsing).
//      The flood is deterministic (sorted-coord order) and bounded by
//      kMaxSupportFloodNodes, so the unstable set is byte-identical across runs.
//
// Single composite level (M13). Detection operates on exactly one level: child
// edits → their immediate parent composite layer (the layer whose decompose_to
// is the terminal layer) → neighbor cascade at that one level. Re-aggregating
// grandparents/root up the chain is deliberately deferred — see the M17 TODO in
// findUnstable(). A non-block-game stack with no such composite layer leaves the
// system inert (active() == false): onVoxelModified no-ops, findUnstable empty.
// ---------------------------------------------------------------------------

namespace sim {

// VoxelCoordLess (the dirty set / support flood / unstable result order) now
// lives in NeighborWalk.h, shared with the M14 field passes (see the M14
// cleanup task in docs/ARCHITECTURE.md §17).

class PropagationSystem {
public:
    // Discovers the single M13 composite level from the world's layer stack: the
    // child layer is the terminal (player-editable) layer, the composite layer is
    // the one whose decompose_to names it. Immutable layers are recorded as anchor
    // sources. If no such composite layer exists the system is inert (active()).
    explicit PropagationSystem(const World& world);

    // True when a composite→terminal level was found and detection is meaningful.
    bool active() const { return composite_ != nullptr && child_ != nullptr; }

    // Edit-path hook (O(1)). Call once per terminal-layer edit at the
    // on_voxel_modified choke point. Applies the old→new structural_strength delta
    // to the parent macro's running aggregate (if a baseline exists) and marks the
    // macro dirty for the next end-of-frame pass. Never re-sums children inline —
    // when no baseline exists yet the delta is dropped and the driver's bounded
    // recomputeAggregate establishes it from current world state instead.
    void onVoxelModified(const WorldCoord& pos,
                         const Voxel& oldVoxel, const Voxel& newVoxel);

    bool hasDirty() const { return !dirty_.empty(); }

    // Remove and return the dirty macro set in deterministic sorted-coord order.
    // The driver drains this each end-of-frame to know which macros to re-evaluate.
    std::vector<chunkmath::VoxelCoord> drainDirty();

    // Bounded full re-sum fallback (O(ratio³)): (re)establish a decomposed macro's
    // aggregate baseline from its current resident child voxels. The driver calls
    // this for dirty macros up to kMaxAggregateRecomputesPerFrame. A no-op for an
    // atomic macro (its aggregate is read directly from its own block material).
    void recomputeAggregate(chunkmath::VoxelCoord macro);

    // Aggregate structural_strength of a macro: an atomic macro reports its own
    // block material; a decomposed macro reports its incremental running average
    // (or a fresh child sum when no baseline is cached); a non-resident or empty
    // macro reports 0.
    float aggregateStrength(chunkmath::VoxelCoord macro) const;

    // A macro the flood found can no longer reach support, with the fields the
    // StructuralEvent payload needs. support_potential is the residual potential
    // (≤ 0 for an unstable macro; a large negative sentinel when no anchor was
    // reachable at all within the flood bound).
    struct Unstable {
        chunkmath::VoxelCoord macro;
        float aggregate_strength = 0.0f;
        float support_potential  = 0.0f;
    };

    // Run the support-potential flood seeded from the given candidate macros (the
    // drained dirty set) plus their solid 6-neighbors (the single-level neighbor
    // cascade), and return the unstable macros in deterministic sorted-coord
    // order. Reads world residency + properties; writes nothing.
    std::vector<Unstable> findUnstable(
        const std::vector<chunkmath::VoxelCoord>& candidates) const;

    // ── Geometry helpers (used by the driver to build StructuralEvent) ─────────
    const Layer* compositeLayer() const { return composite_; }
    const Layer* terminalLayer()  const { return child_; }
    double       macroVoxelSizeM() const;
    WorldCoord   macroCenter(chunkmath::VoxelCoord macro) const;
    // The composite macro voxel that owns the terminal-layer edit at pos.
    chunkmath::VoxelCoord parentMacro(const WorldCoord& pos) const;

private:
    using AggMemo = std::unordered_map<chunkmath::VoxelCoord, double,
                                       chunkmath::VoxelCoordHash>;

    // Cheap classification (chunk residency + composite block voxel) without
    // touching children.
    struct MacroInfo {
        bool  resident     = false;
        bool  atomic       = false;  // resident, undecomposed solid block
        float blockStrength = 0.0f;  // valid when atomic
    };
    MacroInfo classify(chunkmath::VoxelCoord macro) const;

    // Σ of child structural_strength over a decomposed macro's full ratio³ cells.
    double childStrengthSum(chunkmath::VoxelCoord macro) const;
    // Cached (incremental aggregates_, else flood memo) or fresh child sum.
    double cachedSum(chunkmath::VoxelCoord macro, AggMemo& memo) const;

    bool   macroResident(chunkmath::VoxelCoord macro) const;
    bool   macroSolid(chunkmath::VoxelCoord macro, AggMemo& memo) const;
    double macroAggregate(chunkmath::VoxelCoord macro, AggMemo& memo) const;
    // True if the macro at this coord is an anchor source (non-resident boundary
    // or an immutable-layer voxel at its center).
    bool   isAnchor(chunkmath::VoxelCoord macro) const;

    const World& world_;
    const Layer* composite_ = nullptr;   // the M13 composite layer (owned by World)
    const Layer* child_     = nullptr;   // its decompose_to = the terminal layer
    std::vector<const Layer*> immutableLayers_;
    int64_t ratio_ = 1;                  // child voxels per composite voxel edge

    // Running Σ(child structural_strength) per decomposed macro (incremental).
    std::unordered_map<chunkmath::VoxelCoord, double, chunkmath::VoxelCoordHash>
        aggregates_;
    // Macros touched since the last drainDirty(), kept ordered for determinism.
    std::set<chunkmath::VoxelCoord, VoxelCoordLess> dirty_;
};

}  // namespace sim

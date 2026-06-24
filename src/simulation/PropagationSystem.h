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
// PropagationSystem — the DETECTION half of M13/M17 upward damage propagation
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
//      the average, which is the whole point (§7 "Purpose"). At level 0 (the
//      composite layer whose decompose_to is the terminal layer) the children are
//      terminal voxels read by value. At a coarser level the "children" are the
//      next-finer composite layer's macros, and a child's effective strength is
//      *its own aggregate* — so hollowing a terminal voxel lowers its parent
//      macro's aggregate, which lowers its grandparent's, all the way up the
//      chain. The level-0 running Σ is maintained O(1) per edit from the
//      on_voxel_modified old→new delta (onVoxelModified); a full re-sum
//      (recomputeAggregate) is the bounded fallback, and recomputing a level also
//      marks its parent macro dirty so the next-coarser level re-aggregates.
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
//      It runs independently at every composite level.
//
// Multi-level chain (M17, gap audit G1). Detection operates on the full ancestor
// chain the M10 cascade computes: level 0 is the immediate parent of the terminal
// layer; each coarser ancestor (the layer whose decompose_to is the prior level's
// composite) is a higher level. A grandchild edit re-evaluates not just its
// immediate parent but every ancestor macro, so a deep stack (demo's macro→micro→
// grid) collapses a grandparent macro once enough grandchildren are hollowed out.
// A non-block-game stack with no composite layer leaves the system inert
// (active() == false): onVoxelModified no-ops, findUnstable empty.
// ---------------------------------------------------------------------------

namespace sim {

// VoxelCoordLess (the dirty set / support flood / unstable result order) now
// lives in NeighborWalk.h, shared with the M14 field passes (see the M14
// cleanup task in docs/ARCHITECTURE.md §17).

class PropagationSystem {
public:
    // Discovers the composite-level chain from the world's layer stack: the child
    // (terminal) layer is the player-editable one, level 0 is the layer whose
    // decompose_to names it, and each coarser ancestor (the layer whose
    // decompose_to names the prior level's composite) is the next level up.
    // Immutable layers are recorded as anchor sources. If no composite layer
    // exists the system is inert (active()).
    explicit PropagationSystem(const World& world);

    // True when at least one composite→child level was found and detection is
    // meaningful.
    bool active() const { return !levels_.empty(); }

    // Number of composite levels in the chain (0 when inactive). Level 0 is the
    // immediate parent of the terminal layer; higher indices are coarser.
    int levelCount() const { return static_cast<int>(levels_.size()); }

    // Edit-path hook (O(1)). Call once per terminal-layer edit at the
    // on_voxel_modified choke point. Applies the old→new structural_strength delta
    // to the parent macro's (level-0) running aggregate (if a baseline exists) and
    // marks the macro dirty for the next end-of-frame pass. Never re-sums children
    // inline — when no baseline exists yet the delta is dropped and the driver's
    // bounded recomputeAggregate establishes it from current world state instead.
    void onVoxelModified(const WorldCoord& pos,
                         const Voxel& oldVoxel, const Voxel& newVoxel);

    // True if any level has dirty macros awaiting re-evaluation.
    bool hasDirty() const;

    // Remove and return a level's dirty macro set in deterministic sorted-coord
    // order. The driver drains each level (fine→coarse) every end-of-frame to know
    // which macros to re-evaluate. Defaults to level 0.
    std::vector<chunkmath::VoxelCoord> drainDirty(int level = 0);

    // Bounded full re-sum fallback (O(ratio³)): (re)establish a decomposed macro's
    // aggregate baseline at the given level from its current resident children
    // (terminal voxels at level 0, child-macro aggregates above), then mark the
    // macro's parent at the next-coarser level dirty — the upward cascade that
    // makes a grandchild edit re-evaluate its ancestors. The driver calls this for
    // dirty macros up to kMaxAggregateRecomputesPerFrame across all levels. A no-op
    // baseline-wise for an atomic macro (its aggregate is its own block material).
    void recomputeAggregate(chunkmath::VoxelCoord macro) { recomputeAggregate(0, macro); }
    void recomputeAggregate(int level, chunkmath::VoxelCoord macro);

    // Aggregate structural_strength of a macro at the given level (default 0): an
    // atomic macro reports its own block material; a decomposed macro reports its
    // incremental running average (or a fresh child aggregate sum when no baseline
    // is cached); a non-resident or empty macro reports 0.
    float aggregateStrength(chunkmath::VoxelCoord macro) const {
        return aggregateStrength(0, macro);
    }
    float aggregateStrength(int level, chunkmath::VoxelCoord macro) const;

    // A macro the flood found can no longer reach support, with the fields the
    // StructuralEvent payload needs. support_potential is the residual potential
    // (≤ 0 for an unstable macro; a large negative sentinel when no anchor was
    // reachable at all within the flood bound).
    struct Unstable {
        chunkmath::VoxelCoord macro;
        float aggregate_strength = 0.0f;
        float support_potential  = 0.0f;
    };

    // Run the support-potential flood at a level (default 0) seeded from the given
    // candidate macros (the drained dirty set) plus their solid 6-neighbors (the
    // neighbor cascade), and return the unstable macros in deterministic
    // sorted-coord order. Reads world residency + properties; writes nothing.
    std::vector<Unstable> findUnstable(
        const std::vector<chunkmath::VoxelCoord>& candidates) const {
        return findUnstable(0, candidates);
    }
    std::vector<Unstable> findUnstable(
        int level, const std::vector<chunkmath::VoxelCoord>& candidates) const;

    // ── Geometry helpers (used by the driver to build StructuralEvent) ─────────
    const Layer* compositeLayer(int level = 0) const;
    const Layer* terminalLayer()  const { return child_; }
    // The given level's decompose child: the terminal layer at level 0, the
    // next-finer composite layer above it.
    const Layer* childLayerAt(int level) const;
    double       macroVoxelSizeM(int level = 0) const;
    double       childVoxelSizeM(int level = 0) const;
    WorldCoord   macroCenter(chunkmath::VoxelCoord macro) const {
        return macroCenter(0, macro);
    }
    WorldCoord   macroCenter(int level, chunkmath::VoxelCoord macro) const;
    // The level-0 composite macro voxel that owns the terminal-layer edit at pos.
    chunkmath::VoxelCoord parentMacro(const WorldCoord& pos) const;

private:
    using AggMemo = std::unordered_map<chunkmath::VoxelCoord, double,
                                       chunkmath::VoxelCoordHash>;
    // Per-call aggregate cache, one map per level so a macro coord that repeats
    // across levels never collides — each level's macro is summed at most once.
    struct Memo { std::vector<AggMemo> level; };
    Memo makeMemo() const {
        Memo m;
        m.level.resize(levels_.size());
        return m;
    }

    // Cheap classification (chunk residency + composite block voxel) without
    // touching children, at the given level.
    struct MacroInfo {
        bool  resident      = false;
        bool  atomic        = false;  // resident, undecomposed solid block
        float blockStrength = 0.0f;   // valid when atomic
    };
    MacroInfo classify(int level, chunkmath::VoxelCoord macro) const;

    // Effective structural_strength of one child cell of a level's macro: the
    // terminal voxel's material at level 0, the child macro's own aggregate above.
    double childEffectiveStrength(int level, chunkmath::VoxelCoord childVoxel,
                                  Memo& memo) const;
    // Σ of child effective strength over a decomposed macro's full ratio³ cells.
    double childStrengthSum(int level, chunkmath::VoxelCoord macro, Memo& memo) const;
    // Cached (incremental aggregates, else memo) or fresh child sum.
    double cachedSum(int level, chunkmath::VoxelCoord macro, Memo& memo) const;

    bool   macroResident(int level, chunkmath::VoxelCoord macro) const;
    bool   macroSolid(int level, chunkmath::VoxelCoord macro, Memo& memo) const;
    double macroAggregate(int level, chunkmath::VoxelCoord macro, Memo& memo) const;
    // True if the macro at this coord/level is an anchor source (non-resident
    // boundary or an immutable-layer voxel at its center).
    bool   isAnchor(int level, chunkmath::VoxelCoord macro) const;

    // One composite level of the ancestor chain.
    struct Level {
        const Layer* composite = nullptr;  // this level's composite layer (owned by World)
        const Layer* child     = nullptr;  // its decompose_to child (terminal at level 0)
        int64_t      ratio     = 1;        // child voxels per composite voxel edge
        // Running Σ(child effective strength) per decomposed macro (incremental).
        std::unordered_map<chunkmath::VoxelCoord, double, chunkmath::VoxelCoordHash>
            aggregates;
        // Macros touched since the last drainDirty(level), kept ordered for
        // determinism. Seeded by edits at level 0 and by child recomputes above.
        std::set<chunkmath::VoxelCoord, VoxelCoordLess> dirty;
    };

    const World& world_;
    const Layer* child_ = nullptr;  // the terminal layer (level 0's child)
    std::vector<const Layer*> immutableLayers_;
    // [0] = the finest composite (child = terminal); each higher index is the
    // coarser ancestor whose decompose_to is the prior level's composite.
    std::vector<Level> levels_;
};

}  // namespace sim

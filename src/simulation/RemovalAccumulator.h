#pragma once

#include "simulation/RemovalModel.h"
#include "world/ChunkCoordMath.h"  // chunkmath::VoxelCoord

// ---------------------------------------------------------------------------
// Per-target removal accumulator — the transient tool state that turns the
// pure, stateless RemovalModel into a held-to-mine interaction (M8).
//
// The build/break tool feeds one tick per frame while the remove action is
// held on a targeted voxel. The accumulator accrues `power * dt` work-units
// against that voxel and reports when the accrued work reaches the voxel's
// hardness-derived threshold (`removalWork(hardness)`), at which point the
// caller clears the voxel and fires `on_voxel_modified` exactly once. Progress
// is reset when the targeted voxel changes (a new target starts from zero) and
// is dropped by `reset()` when the action is released or nothing is targeted.
//
// This is *transient* state: it lives in the tool/demo path, is never
// persisted, and never touches saved world state. Dirty tracking stays
// chunk-granular (ARCHITECTURE.md §5, §9). The removal *outcome* is fully
// deterministic (it comes from RemovalModel); only the wall-clock time to
// reach it depends on frame rate and how long the action is held.
//
// Property-driven, never id-driven: callers pass the `hardness` read off the
// target voxel's own `MaterialProperties` (ARCHITECTURE.md §5), never a
// material id. An indestructible target (`hardness < 0`) or a powerless tool
// (`power <= 0`) never accrues progress and is never cleared.
// ---------------------------------------------------------------------------

namespace sim {

class RemovalAccumulator {
public:
    // Accrue one tick of removal work against `target`. `hardness` is read off
    // the target voxel's material; `power` is the tool's work-units per second;
    // `dt` is the elapsed seconds for this tick.
    //
    // Switching to a different target (or first contact with any target) resets
    // progress to zero before accruing. Returns true exactly on the tick the
    // accrued work first reaches the hardness-derived threshold — the caller
    // clears the voxel then and should follow with reset(). Returns false (and
    // accrues nothing) for an indestructible target (hardness < 0) or a
    // powerless tool (power <= 0).
    bool accrue(const chunkmath::VoxelCoord& target, float hardness, float power, float dt);

    // Forget the current target and drop all progress. Call when the remove
    // action is released, nothing is targeted, or right after a clear.
    void reset();

    bool hasTarget() const { return has_target_; }

    // The voxel currently being removed (only meaningful when hasTarget()).
    const chunkmath::VoxelCoord& target() const { return target_; }

    // Progress toward removal in [0, 1]. Zero when there is no target or the
    // target is not removable (indestructible / powerless tool). Used by the
    // removal-feedback path (crack stages / color ramp).
    float progress() const;

private:
    bool                  has_target_ = false;
    chunkmath::VoxelCoord  target_{};
    float                 accrued_   = 0.0f;  // work-units accrued on target_
    float                 threshold_ = 0.0f;  // removalWork(hardness) for target_
    bool                  removable_ = false; // isRemovable(hardness, power) for target_
};

}  // namespace sim

#pragma once

#include "plugin_api.h"  // MaterialProperties

// ---------------------------------------------------------------------------
// Voxel-removal cost model — the first consumer under src/simulation/ (M8).
//
// The effort to remove a voxel is a pure, deterministic function of its
// `hardness` and the removing tool's `power`. There is no block-type id and no
// live material-registry lookup here: a consumer reads `voxel.material.hardness`
// off the voxel by value and passes it in (ARCHITECTURE.md §5, the consumption
// contract). This file depends only on MaterialProperties — not on World, the
// renderer, or PluginManager — so it is unit-testable in isolation.
//
//   hardness  > 0  -> work proportional to hardness; time = hardness / power
//   hardness == 0  -> no resistance; removes in a single step (fail-soft default)
//   hardness  < 0  -> indestructible sentinel; never removable
//   power    <= 0  -> tool can never remove anything (infinite time)
//
// The *outcome* (removable or not) is fully deterministic. Only the wall-clock
// time to reach it depends on frame rate and how long the action is held, and
// that timing never touches saved world state.
// ---------------------------------------------------------------------------

namespace sim {

// Indestructibility sentinel: any negative hardness marks a voxel that no tool
// can ever clear. -1.0f by convention; the actual test everywhere is
// `hardness < 0`, so any negative value round-trips identically (ARCHITECTURE §5).
inline constexpr float kIndestructible = -1.0f;

// True when a tool of the given power can ever remove a voxel of the given
// hardness. False for an indestructible voxel (hardness < 0) or a powerless
// tool (power <= 0). This is the gate every removal consumer checks first.
constexpr bool isRemovable(float hardness, float power) {
    return hardness >= 0.0f && power > 0.0f;
}

// Work-units required to remove a voxel of the given hardness. For hardness > 0
// this is the hardness itself — the mapping is linear and transparent, so a
// harder material requires strictly more work. For hardness == 0 it is zero
// (instant removal). An indestructible voxel (hardness < 0) has no finite
// threshold; this returns +infinity, so callers must gate on isRemovable first.
float removalWork(float hardness);

// Time in seconds to remove, given tool power in work-units per second
// (time = removalWork(hardness) / power). Returns +infinity when the target is
// not removable — an indestructible voxel (hardness < 0) or a powerless tool
// (power <= 0). Strictly increasing in hardness for a fixed power > 0.
float removalTime(float hardness, float power);

// MaterialProperties convenience overloads — read hardness off the voxel's own
// material record, never a material id (the consumption contract, §5).
inline bool isRemovable(const MaterialProperties& material, float power) {
    return isRemovable(material.hardness, power);
}
inline float removalWork(const MaterialProperties& material) {
    return removalWork(material.hardness);
}
inline float removalTime(const MaterialProperties& material, float power) {
    return removalTime(material.hardness, power);
}

}  // namespace sim

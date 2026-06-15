#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Central engine tuning knobs.
//
// This header is the single home for *tunable* engine constants — the values a
// developer turns to change feel or performance (budgets, radii, spans,
// thresholds). It is header-only `inline constexpr` and depends on nothing but
// the standard library, so any tier may include it without crossing a §13
// dependency boundary: it introduces pure values, never behavioral coupling.
//
// What belongs here vs. what does not:
//   - HERE: knobs you would plausibly retune — per-frame budgets, streaming
//     radii, the structural support span. Grouped by subsystem namespace.
//   - NOT here: contract constants / sentinels that define a format or an
//     invariant rather than a tuning choice (e.g. `sim::kIndestructible`, the
//     256-entry palette size). Those stay with the model they belong to.
//
// Migration note: existing scattered knobs (the DecompositionManager::tick
// default budgets, LODManager's streaming radii) should move here under
// `tuning::decomposition` / `tuning::streaming` in a small dedicated pass,
// kept separate from feature work.
// ---------------------------------------------------------------------------

namespace tuning::physics {

// Structural support model (M13, docs/architecture.md §7).
//
// A decomposed macro voxel is stable iff support potential floods to it from an
// anchor (an immutable voxel, or the conservative unloaded-region boundary)
// through solid macro voxels without draining to zero. Entering a macro of
// aggregate `structural_strength` s drains potential by 1 / maxSpan(s), where
//   maxSpan(s) = clamp(s * kSupportSpanPerStrength, 0, kMaxSupportSpan)
// is, in macro-voxels, the unsupported span that material can bridge. A macro
// with residual potential <= 0 is unstable and fires a structural event.

// Macro-voxels of unsupported span a material can bridge per unit
// structural_strength. Calibrated against the registered material scale
// (stone ~0.9 -> ~5 voxels, diamond ~2.0 -> ~10).
inline constexpr float kSupportSpanPerStrength = 5.0f;

// Hard cap on bridgeable span (macro-voxels). Bounds the support flood radius
// so a single event's connectivity query stays small.
inline constexpr int kMaxSupportSpan = 16;

// Aggregate strength below which a macro transmits no support at all (water,
// lava, and rubble cannot hold anything up). Floors the flood so near-zero
// strength does not produce an unbounded reach.
inline constexpr float kMinSupportStrength = 0.05f;

// Support potential emitted by an anchor. Normalized to 1.0; potential drains
// by 1 / maxSpan(s) per macro step and a macro is stable while it stays > 0.
inline constexpr float kAnchorPotential = 1.0f;

// Per-frame budgets for the deferred end-of-frame propagation pass. Aggregates
// are maintained incrementally (a running volume-weighted sum updated by the
// on_voxel_modified delta), so full re-sums are a bounded fallback, not the
// common path. Events beyond the cap carry to the next frame, spreading a
// cascade across frames instead of stalling one.
inline constexpr int kMaxAggregateRecomputesPerFrame = 64;
inline constexpr int kMaxStructuralEventsPerFrame     = 256;
inline constexpr int kMaxSupportFloodNodes            = 4096;

}  // namespace tuning::physics

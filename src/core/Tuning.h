#pragma once

#include <cstdint>

#include "core/EngineConfig.h"

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
// PER-FRAME BUDGETS ARE NOW RUNTIME-SETTABLE (M17 D3b). The work-budget caps
// below are no longer the live values the subsystems read — they are derived
// from `EngineConfig`'s defaults and kept as the documented compile-time
// baseline (tests reference them). The value actually used each tick comes from
// the runtime `engineConfig()` store; see include/core/EngineConfig.h. The
// model constants (support span, stability factor, attenuation, …) remain
// compile-time.
// ---------------------------------------------------------------------------

namespace tuning::decomposition {

// Default per-frame budgets for DecompositionManager::tick (M10). They cap how
// much streaming/decomposition work a single tick may do, so a burst of newly
// in-range macro voxels is spread across frames instead of hitching one. These
// are the function's default arguments; a caller may still pass its own.
//
//   kDefaultLoadPerFrame   — max composite chunks loaded per tick
//   kDefaultDecompPerFrame — max decomposition jobs enqueued per tick (nearest-first)
//   kDefaultApplyPerFrame  — max completed jobs applied per tick (overflow stays queued)
inline constexpr int kDefaultLoadPerFrame   = EngineConfig{}.decompositionLoadPerFrame;
inline constexpr int kDefaultDecompPerFrame = EngineConfig{}.decompositionDecompPerFrame;
inline constexpr int kDefaultApplyPerFrame  = EngineConfig{}.decompositionApplyPerFrame;

}  // namespace tuning::decomposition

namespace tuning::streaming {

// Hysteresis margin (chunks) added to a layer's load radius to get its eviction
// radius. A chunk loads at the view distance but is not evicted until it passes
// view distance + this margin, so a camera hovering on the boundary does not
// thrash a chunk between resident and evicted (LODManager).
inline constexpr int kHysteresisChunks = EngineConfig{}.streamingHysteresisChunks;

}  // namespace tuning::streaming

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
inline constexpr int kMaxAggregateRecomputesPerFrame = EngineConfig{}.physicsMaxAggregateRecomputesPerFrame;
inline constexpr int kMaxStructuralEventsPerFrame     = EngineConfig{}.physicsMaxStructuralEventsPerFrame;
inline constexpr int kMaxSupportFloodNodes            = EngineConfig{}.physicsMaxSupportFloodNodes;

}  // namespace tuning::physics

namespace tuning::thermal {

// Heat diffusion model (M14, docs/architecture.md §17).
//
// ThermalSystem advances an engine-owned sparse temperature overlay with
// explicit finite-difference diffusion: dT/dt = a*Laplacian(T), where the
// per-cell coefficient a is read from that cell's thermal_conductivity. The
// explicit scheme is only conditionally stable, so the pass sub-steps within
// the frame to respect kStabilityFactor rather than taking one large step.

// Default/absent-cell value of the temperature overlay. A cell not present in
// the sparse store reads as this — the "room temperature" floor everything
// decays toward once no nearby heat source keeps it elevated.
inline constexpr float kAmbientTemperature = 20.0f;

// The explicit-scheme stability bound, 3D case: a*dt/dx^2 <= kStabilityFactor.
// Drives how many sub-steps a tick's diffusion pass needs for the most
// conductive cell touched; exceeding this bound oscillates/blows up.
inline constexpr float kStabilityFactor = 1.0f / 6.0f;

// Per-frame budget on distinct cells the diffusion pass visits (active set +
// frontier, sorted-coord order). Unlike the M13 dirty queue this needs no
// explicit carry bookkeeping: a cell skipped this frame simply stays in the
// overlay's own active set/frontier and is reconsidered next tick.
inline constexpr int kMaxThermalCellsPerFrame = EngineConfig{}.thermalMaxCellsPerFrame;

}  // namespace tuning::thermal

namespace tuning::fluid {

// Cellular-automaton fluid flow model (M14, docs/architecture.md §17).
//
// FluidSystem advances an engine-owned sparse fluid-amount overlay with
// level/head cellular-automaton flow gated by each destination cell's
// porosity (0 blocks entirely, 1 — and air/empty, treated as effective 1.0 —
// is fully permeable).

// Fluid amount at which a cell crosses into "realized geometry": the engine
// fires on_fluid_event(rising) so the mandatory flow plugin can place a real
// voxel through the public edit path.
inline constexpr float kSaturationThreshold = 1.0f;

// Fluid amount below which an already-realized cell fires
// on_fluid_event(falling) so the flow plugin clears its voxel. Strictly below
// kSaturationThreshold so a cell does not flicker rising/falling at the same
// value.
inline constexpr float kMinFluidAmount = 0.05f;

// Per-frame budgets for the end-of-frame flow pass. Overflow beyond either cap
// carries to the next frame (the tuning::physics carry pattern), so a large
// release spreads across frames instead of stalling one.
inline constexpr int kMaxFluidCellsPerFrame  = EngineConfig{}.fluidMaxCellsPerFrame;
inline constexpr int kMaxFluidEventsPerFrame = EngineConfig{}.fluidMaxEventsPerFrame;

}  // namespace tuning::fluid

namespace tuning::lighting {

// Sky light + block (emitter) light model (M17, docs/ARCHITECTURE.md §17).
//
// LightingSystem owns a sparse FieldOverlay of combined brightness at
// terminal-voxel granularity. Block light propagates via BFS from emitters
// (materials with light_emission > 0); sky light floods downward from
// unobstructed columns. Both are clamped to [0, kMaxBrightness].

// Ambient floor: the minimum brightness every voxel receives even in total
// darkness. 0 means unlit areas are pitch black; raise for a "you can
// always see" baseline.
inline constexpr float kAmbientBrightness = 0.05f;

// Maximum brightness value (full sky light or a maximum-power emitter).
inline constexpr float kMaxBrightness = 1.0f;

// Block-light attenuation per voxel step: each BFS hop reduces the
// propagated brightness by this fraction of kMaxBrightness, so the
// effective range of a full-power emitter is kMaxBrightness / kAttenuationPerStep.
inline constexpr float kAttenuationPerStep = 1.0f / 15.0f;

// Per-frame budget on distinct cells the lighting pass visits (active set +
// frontier, sorted-coord order). Same carry convention as thermal/fluid:
// a cell skipped this frame stays in the overlay's active set for next tick.
inline constexpr int kMaxLightingCellsPerFrame = EngineConfig{}.lightingMaxCellsPerFrame;

// Per-frame budget on lighting events fired (rising/falling boundary
// crossings). Overflow carries to the next frame.
inline constexpr int kMaxLightingEventsPerFrame = EngineConfig{}.lightingMaxEventsPerFrame;

}  // namespace tuning::lighting

namespace tuning::ao {

// Per-vertex ambient occlusion in the chunk mesher (M17, sanity-check A2).
//
// "Smooth lighting": each face vertex is darkened by how enclosed its concave
// corner is. The mesher counts opaque voxels in the 2x2 neighborhood around the
// vertex in the voxel layer just OUTSIDE the face (the air side) and derives an
// occlusion level 0..3 (3 = fully open corner, 0 = a corner boxed in on two
// sides). This is a purely mesher-local computation — no overlay, no new data —
// so it costs nothing at runtime beyond the per-vertex multiply baked into the
// vertex color, alongside the fixed directional shade and the optional light.

// Brightness multiplier indexed by occlusion level. kVertexFactor[3] is a fully
// open corner (1.0 — no darkening, so flat/convex terrain is unchanged); lower
// levels darken progressively toward a corner enclosed on multiple sides. Set
// every entry to 1.0 to disable AO without touching the mesher.
inline constexpr float kVertexFactor[4] = {0.46f, 0.64f, 0.82f, 1.0f};

}  // namespace tuning::ao

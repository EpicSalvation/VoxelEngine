#pragma once

// ---------------------------------------------------------------------------
// Runtime-adjustable engine configuration (M17 D3b).
//
// These are the engine's per-frame WORK BUDGETS — the caps on how much
// streaming/decomposition/simulation a single tick may do, so a burst of work
// spreads across frames instead of hitching one. Before M17 they were the
// compile-time `inline constexpr` constants in src/core/Tuning.h, so retuning
// them (or shipping an in-game quality slider) meant a rebuild. Promoting them
// to a runtime struct lets a game trade convergence speed for frame pacing on
// weaker hardware, and lets a developer tweak without recompiling.
//
// Every field defaults to the exact value it held as a Tuning.h constant, so an
// engine that never touches engineConfig() is byte-for-byte identical to the
// pre-D3b build. Tuning.h still publishes those numbers as named constants —
// now DERIVED from these defaults, so this struct is the single source of truth
// — which the tests and docs reference as the documented baseline.
//
// Scope: these caps are per-frame and per-subsystem, not per-world, so the
// runtime store is a process-global singleton (engineConfig()) rather than a
// LayerConfig field. Each simulation subsystem reads it on the main-loop thread
// during its tick; set it before or between ticks. It is plain configuration,
// not world state — outside the determinism contract (a run is deterministic as
// long as the config is stable across that run).
//
// This header is intentionally dependency-free (a POD aggregate + two free
// functions) so it can sit in the PUBLIC include/ surface and be reached by an
// out-of-tree game the same way LayerConfig is.
// ---------------------------------------------------------------------------

struct EngineConfig {
    // ── Decomposition / streaming throughput (DecompositionManager::tick) ──
    int decompositionLoadPerFrame   = 4;   // max composite chunks loaded per tick
    int decompositionDecompPerFrame = 64;  // max decomposition jobs enqueued per tick (nearest-first)
    int decompositionApplyPerFrame  = 16;  // max completed jobs applied per tick (overflow stays queued)

    // ── Streaming residency hysteresis (LODManager) ──
    // Margin (chunks) between a layer's load radius and its larger eviction
    // radius, so a camera hovering on the boundary does not thrash a chunk.
    int streamingHysteresisChunks = 2;

    // ── Structural propagation (PhysicsSystem / PropagationSystem) ──
    int physicsMaxAggregateRecomputesPerFrame = 64;    // aggregate re-sums per frame
    int physicsMaxStructuralEventsPerFrame     = 256;  // structural events fired per frame
    int physicsMaxSupportFloodNodes            = 4096; // support-flood nodes per event

    // ── Heat diffusion (ThermalSystem) ──
    int thermalMaxCellsPerFrame = 4096;  // diffusion cells visited per frame

    // ── Fluid flow (FluidSystem) ──
    int fluidMaxCellsPerFrame  = 4096;  // flow cells visited per frame
    int fluidMaxEventsPerFrame = 256;   // fluid rising/falling events fired per frame

    // ── Sky/block light propagation (LightingSystem) ──
    int lightingMaxCellsPerFrame  = 8192;  // lighting cells visited per frame
    int lightingMaxEventsPerFrame = 256;   // lighting events fired per frame
};

// Process-global runtime config. Defaults to the baseline above; mutate it for a
// quality slider or dev-time tuning. Read by the simulation subsystems each tick.
EngineConfig& engineConfig();

// Restore every field to its default — a "reset to defaults" action for a UI, and
// the way a test that mutated the config leaves it clean for the next test.
void resetEngineConfig();

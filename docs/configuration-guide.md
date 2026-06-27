# Configuration Guide

A single reference for every tunable knob the engine exposes — what it does, its
default, and when you would change it. The engine deliberately splits configuration
across three surfaces by *how often and by whom* a value is set:

| Surface | Lives in | Set at | Who sets it |
|---------|----------|--------|-------------|
| **A. World shape** | a YAML layer config (`LayerConfig`) | startup, per project | game / world author |
| **B. Engine model constants** | `src/core/Tuning.h` (compile-time `constexpr`) | build time | engine integrator |
| **C. Runtime APIs** | per-subsystem setters + `engineConfig()` | any time while running | game code, per frame |

If you are configuring a *world* (how big voxels are, how far they stream, where the
player edits) you want **A**. If you are retuning a simulation *model* (collapse reach,
diffusion stability, light attenuation) you want **B**. If you are driving the engine
*live* — frame rate cap, camera, audio listener, log verbosity, or the **per-frame
work budgets** (§C.6) — you want **C**.

> **Note (M17 D3b landed).** The per-frame work budgets were promoted from compile-time
> constants to a runtime struct (`EngineConfig`). They are now configured through the **Per-frame work budgets** API in §C,
> not by rebuilding. `Tuning.h` still publishes them as named constants — now *derived*
> from the `EngineConfig` defaults — as the documented baseline, and the §B budget rows
> below note their runtime field. Only the genuine *model* constants in §B still require
> a rebuild.

---

## A. Layer config (YAML) — the world's shape

A world is defined by a YAML file (or inline string) describing a **stack of layers**
from coarsest to finest. Loaded via `LayerConfig::loadFromFile(path)` or
`loadFromString(yaml)` (`include/core/LayerConfig.h`). All validation runs at load
time and throws `std::runtime_error` with a descriptive message — an invalid world
fails fast at startup, never silently at runtime.

### Per-layer fields

Each entry under `layers:` accepts:

| Field | Type | Default | Required | When you'd change it |
|-------|------|---------|----------|----------------------|
| `name` | string | — | **yes** | Identifies the layer; referenced by `decompose_to`. |
| `voxel_size_m` | double > 0 | — | **yes** | The edge length of one voxel in this layer, in metres. The whole stack's scale. |
| `mode` | `composite` \| `immutable` \| `terminal` | — | **yes** | `composite` = lazily decomposes into a finer child; `terminal` = the editable leaf; `immutable` = collision+render only, never modified or persisted. |
| `decompose_to` | string | — | composite only | Names the child layer this layer decomposes into. Must exist in the stack. |
| `interactive` | bool | `false` | no | Flags the one layer the single-layer World API (get/setVoxel, dirty tracking, persistence, picking) targets. At most one layer may set it; omit to accept the default (first terminal, else first layer). Set it to put the playspace mid-stack. |
| `chunk_size_voxels` | int ≥ 1 | `32` | no | Voxels per chunk side (grid is the cube of this). Lower for finely decomposed layers (shipped configs go as low as 4) to keep each chunk cheap; higher to amortize per-chunk overhead. |
| `view_distance_chunks` | int ≥ 0 | `8` | no | Load/evict radius around the camera, in chunks. Also the streaming-volume radius. Raise for longer view distance at higher memory/CPU cost. |
| `resident_chunk_budget` | int ≥ 0 | `0` (unlimited) | no | Hard cap on resident chunks for this layer; the manager evicts farthest-first clean chunks to fit (near and dirty chunks are pinned). Set it to bound memory across a deep stack. |
| `decompose_distance_m` | double ≥ 0 | unset → single-radius fallback | no | Distance at which this layer's macro voxels decompose into their child. Setting it **per layer decouples the cascade**: reveal a coarse silhouette far out yet build the fine, expensive grid only up close (e.g. a 4 m silhouette at 280 m, the 1 m grid only within 90 m). |
| `streaming_volume.shape` | `box` \| `sphere` \| `shell` | `box` | no | Shape of the residency volume. `box` = isotropic cube (reproduces pre-M16 behavior); `sphere` = Euclidean ball; `shell` = thin band resident only at range (distant backdrops). Use non-box for deep-descent, flying, or space worlds with no vertical bias. |
| `streaming_volume.shell_thickness_chunks` | int ≥ 1 | `1` | shell only | Band width of a `shell` volume; inner radius = `view_distance_chunks − this`. |

### Stack-wide validation rules

Enforced across the whole stack at load:

- **At least one layer** must be defined.
- **`voxel_size_m` strictly descending** — each child smaller than its parent.
- **Adjacent ratio is a whole integer ≥ 2** — a parent voxel must tile cleanly into
  child voxels (e.g. 16 m → 4 m is fine; 16 m → 6 m is rejected).
- **Every composite layer's `decompose_to`** must name a layer that exists.
- **At most one `interactive: true`** — two or more is a hard error.
- **Coarse-supersets-fine** — for a composite→composite transition, the parent voxel
  must be ≥ the child layer's chunk world size (`child voxel_size_m × child
  chunk_size_voxels`), so one parent macro fills whole child chunks without overlap.
  Violations throw; fix by lowering the child's `chunk_size_voxels` or raising the
  parent's `voxel_size_m`. (See `m17-g1-multi-level-propagation` for the deep-stack
  constraint this enforces.)

### Example

```yaml
layers:
  - name: macro            # coarsest composite
    voxel_size_m: 16.0
    mode: composite
    decompose_to: micro
    chunk_size_voxels: 4
    view_distance_chunks: 7
    resident_chunk_budget: 3500
    decompose_distance_m: 280.0   # reveal silhouette far out
    streaming_volume:
      shape: box

  - name: micro            # intermediate composite
    voxel_size_m: 4.0
    mode: composite
    decompose_to: grid
    chunk_size_voxels: 4
    view_distance_chunks: 20
    resident_chunk_budget: 4096
    decompose_distance_m: 90.0    # build fine grid only up close

  - name: grid             # editable terminal leaf
    voxel_size_m: 1.0
    mode: terminal
    interactive: true             # the single-layer API targets this layer
    chunk_size_voxels: 4
    view_distance_chunks: 10
    resident_chunk_budget: 6144
```

---

## B. Engine tuning constants (`src/core/Tuning.h`)

`Tuning.h` is the single home for *tunable* engine constants — the values you turn to
change feel or performance. They are header-only `inline constexpr`, grouped by
subsystem namespace. The genuine **model constants** here (support span, stability
factor, attenuation, AO factors, ambient floors) **require a rebuild** to change.
Format/invariant constants (palette size, sentinels) deliberately live with their
model, not here.

The dominant pattern is the **per-frame budget**: a cap on how much work a subsystem's
tick may do, so a burst of work spreads across frames instead of hitching one. Raise a
budget to converge faster (more work per frame, risk of hitches); lower it for
smoother frame pacing at the cost of slower convergence. Overflow carries to the next
frame in every subsystem that has one.

> Any constant below that has a matching field in §C's **Per-frame work budgets** table
> (every per-frame budget, plus the streaming hysteresis margin) is now **runtime-settable**
> via `engineConfig()` — the `Tuning.h` constant is just the compile-time default. That
> table is the authoritative list of what is runtime-tunable; everything else here is a
> rebuild-only *model* constant. Set a runtime value once at startup, or live for a
> quality slider.

### `tuning::decomposition` — streaming/decomposition throughput

| Constant | Default | Meaning |
|----------|---------|---------|
| `kDefaultLoadPerFrame` | `4` | Max composite chunks loaded per tick. |
| `kDefaultDecompPerFrame` | `64` | Max decomposition jobs enqueued per tick (nearest-first). |
| `kDefaultApplyPerFrame` | `16` | Max completed jobs applied per tick (overflow stays queued). |

These are `DecompositionManager::tick`'s default arguments; a caller may pass its own.

### `tuning::streaming` — residency hysteresis

| Constant | Default | Meaning |
|----------|---------|---------|
| `kHysteresisChunks` | `2` | Margin (chunks) between a layer's load radius and its (larger) eviction radius, so a camera on the boundary doesn't thrash chunks in/out. Raise to reduce reload churn at the edge. |

### `tuning::physics` — structural support model (M13)

| Constant | Default | Meaning |
|----------|---------|---------|
| `kSupportSpanPerStrength` | `5.0` | Macro-voxels of unsupported span a material bridges per unit `structural_strength` (stone ~0.9 → ~5 voxels). |
| `kMaxSupportSpan` | `16` | Hard cap on bridgeable span; bounds the support-flood radius. |
| `kMinSupportStrength` | `0.05` | Aggregate strength below which a macro transmits no support (water/lava/rubble hold nothing up). |
| `kAnchorPotential` | `1.0` | Support potential emitted by an anchor; drains by `1/maxSpan` per step, unstable at ≤ 0. |
| `kMaxAggregateRecomputesPerFrame` | `64` | Per-frame cap on aggregate re-sums (budget). |
| `kMaxStructuralEventsPerFrame` | `256` | Per-frame cap on structural events fired (budget). |
| `kMaxSupportFloodNodes` | `4096` | Per-event cap on support-flood connectivity nodes. |

### `tuning::thermal` — heat diffusion (M14)

| Constant | Default | Meaning |
|----------|---------|---------|
| `kAmbientTemperature` | `20.0` | "Room temperature" floor an absent cell reads as / decays toward. |
| `kStabilityFactor` | `1/6` | Explicit-scheme 3D stability bound; drives sub-step count. Lowering is safer/slower, raising risks oscillation. |
| `kMaxThermalCellsPerFrame` | `4096` | Per-frame cap on diffusion cells visited (budget). |

### `tuning::fluid` — cellular-automaton flow (M14)

| Constant | Default | Meaning |
|----------|---------|---------|
| `kSaturationThreshold` | `1.0` | Fluid amount at which a cell realizes geometry (fires `on_fluid_event` rising). |
| `kMinFluidAmount` | `0.05` | Amount below which a realized cell clears (fires falling). Below saturation so cells don't flicker. |
| `kMaxFluidCellsPerFrame` | `4096` | Per-frame cap on flow cells visited (budget). |
| `kMaxFluidEventsPerFrame` | `256` | Per-frame cap on fluid events fired (budget). |

### `tuning::lighting` — sky + block light (M17)

| Constant | Default | Meaning |
|----------|---------|---------|
| `kAmbientBrightness` | `0.05` | Minimum brightness everywhere. `0` = pitch black unlit; raise for a "always see" baseline. |
| `kMaxBrightness` | `1.0` | Full sky light / max-power emitter. |
| `kAttenuationPerStep` | `1/15` | Block-light falloff per voxel hop; emitter range = `kMaxBrightness / kAttenuationPerStep`. |
| `kMaxLightingCellsPerFrame` | `8192` | Per-frame cap on lighting cells visited (budget). |
| `kMaxLightingEventsPerFrame` | `256` | Per-frame cap on lighting events fired (budget). |

### `tuning::ao` — ambient occlusion (M17)

| Constant | Default | Meaning |
|----------|---------|---------|
| `kVertexFactor[4]` | `{0.46, 0.64, 0.82, 1.0}` | Per-vertex brightness multiplier by occlusion level (3 = open corner, no darkening). Set all entries to `1.0` to disable AO without touching the mesher. |

---

## C. Runtime APIs — driven live

These are set through normal C++ calls while the engine runs, typically once at setup
or per frame.

### Engine loop & wiring (`include/core/Engine.h`)

| API | Default | When you'd call it |
|-----|---------|--------------------|
| `setTargetFrameRate(int fps)` / `getTargetFrameRate()` | `60` | Cap the game-loop frame rate; the loop sleeps to hold it. |
| `getMetrics() → EngineMetrics` | — | Per-frame snapshot: `frameTimeSec`, `drawCalls`, `voiceCount`, `decompInFlight`, and per-layer resident-chunk / decomposed-macro counts. Replaces ad-hoc per-demo HUD recomputation; drives HUDs and profiling. |
| `setNetworkManager` / `setAudioManager` / `setFluidSystem` / `setThermalSystem` / `setLightingSystem` / `setRenderer` / `setDecompositionManager` | all `nullptr` | Attach optional subsystems; null disables that subsystem (single-player demos leave most null). Field-system setters also enable the read-only queries below. |
| `temperatureAt` / `fluidAmountAt` / `lightAt(WorldCoord)` | ambient default when unattached | Read-only sampling of the M14/M17 field overlays; returns the absent-cell default when no system is attached. |

### Logging (`src/core/Logger.h`, `namespace Log`)

| API | Default | When you'd call it |
|-----|---------|--------------------|
| `setMinLevel(Level)` | `Info` | Set the verbosity floor (`Debug < Info < Warn < Error`). Drop to `Debug` while diagnosing; raise to `Warn`/`Error` for quiet release runs. |
| `setHandler(Handler)` | stderr writer | Redirect output (tests install a capturing handler; a game might route to its own console/file). `nullptr` restores the default. |
| `debug/info/warn/error(msg)` or `(category, msg)` | — | Emit a message, optionally tagged with a category ("Net", "Physics", …). |

### Audio (`include/plugin_api.h` `SoundParams`, `src/audio/AudioManager.h`)

`SoundParams` is per-playback (passed to `playSound`/`createEmitter`); `EmitterParams`
wraps it for persistent emitters.

| Field | Default | Meaning |
|-------|---------|---------|
| `SoundParams::volume` | `1.0` | Linear gain. |
| `SoundParams::attenuation` | `Inverse` | Distance falloff model: `Inverse` \| `Linear` \| `Exponential` \| `None`. |
| `SoundParams::min_distance` | `1.0` | Distance below which volume is unattenuated. |
| `SoundParams::max_distance` | `100.0` | Distance beyond which attenuation stops scaling. |
| `SoundParams::rolloff` | `1.0` | Falloff steepness. |
| `SoundParams::doppler` | `0.0` | Doppler factor; `0` = off. |
| `EmitterParams::loop` | `true` | Whether a persistent emitter loops. |

Runtime calls: `AudioManager::setListener(pos, forward, up)` each frame to place the
listener; `playSound(id, pos, overrides)` for one-shots; `createEmitter` /
`setEmitterPosition` / `stopEmitter` for persistent positioned sources.

### Renderer (`include/renderer/Renderer.h`)

| API | When you'd call it |
|-----|--------------------|
| `setViewport(width, height)` | On window resize. |
| `setCameraPosition(WorldCoord)` | Each frame to move the view. |
| `setCameraRotation(pitch, yaw, roll)` | Each frame to aim the view. |
| `setCameraUp(vec3 worldUp)` | Each frame on a many-bodied/off-axis world to align the horizon to a surface normal (pass `-gravityDir`). Default `(0,1,0)` is the historical Y-up view, byte-identical (M17). |
| `setFarClip(metres)` | Once (or when view distance changes); default 1000 m. Raise for multi-layer worlds whose coarsest layer spans kilometres. |
| `setFog(FogParams)` | Each frame to drive distance-obscurance fog — color + near/far band + density — hiding the LOD pop and chunk-load edge (M17). Default `density 0` disables fog (byte-identical). Typically fed from a supplier plugin (`atmospheric-mist`, `range-attenuation`); see `include/renderer/Fog.h`. |

### Networking (`src/net/NetworkManager.h`)

| API | Default | When you'd call it |
|-----|---------|--------------------|
| `startServer(port, max_peers)` / `startHostPeer(port, max_peers)` | `max_peers = 32` | Bind as dedicated server or host-peer; cap concurrent connections. |
| `startClient(host, port)` | — | Connect to a server. |
| `setInterestMode(BroadcastAll \| StreamingRadius)` | `BroadcastAll` | Switch edit replication from broadcast-to-all to streaming-radius interest filtering as player counts grow. |
| `setTransport(unique_ptr<ITransport>)` | ENet | Replace the transport backend before starting (testing or a custom transport). |

### Per-frame work budgets (`include/core/EngineConfig.h` `engineConfig()`)

The per-frame work caps from §B (plus the streaming hysteresis margin), promoted to a
runtime struct (M17 D3b) so a game can expose quality sliders and a developer can retune
without a rebuild. Read by each subsystem's tick (or `LODManager` call) from the
process-global `engineConfig()`; mutate before or between ticks. Every field defaults to
its §B `Tuning.h` value, so an engine that never touches it is byte-identical to the
pre-D3b build. `resetEngineConfig()` restores all defaults. There is no per-subsystem
alias for these values — `engineConfig()` is the single place they are read and set.

```cpp
#include "core/EngineConfig.h"
engineConfig().fluidMaxCellsPerFrame = 1024;  // e.g. a "low" quality preset
```

| `EngineConfig` field | Default | §B constant it overrides |
|----------------------|---------|--------------------------|
| `decompositionLoadPerFrame` | `4` | `tuning::decomposition::kDefaultLoadPerFrame` |
| `decompositionDecompPerFrame` | `64` | `tuning::decomposition::kDefaultDecompPerFrame` |
| `decompositionApplyPerFrame` | `16` | `tuning::decomposition::kDefaultApplyPerFrame` |
| `streamingHysteresisChunks` | `2` | `tuning::streaming::kHysteresisChunks` |
| `physicsMaxAggregateRecomputesPerFrame` | `64` | `tuning::physics::kMaxAggregateRecomputesPerFrame` |
| `physicsMaxStructuralEventsPerFrame` | `256` | `tuning::physics::kMaxStructuralEventsPerFrame` |
| `physicsMaxSupportFloodNodes` | `4096` | `tuning::physics::kMaxSupportFloodNodes` |
| `thermalMaxCellsPerFrame` | `4096` | `tuning::thermal::kMaxThermalCellsPerFrame` |
| `fluidMaxCellsPerFrame` | `4096` | `tuning::fluid::kMaxFluidCellsPerFrame` |
| `fluidMaxEventsPerFrame` | `256` | `tuning::fluid::kMaxFluidEventsPerFrame` |
| `lightingMaxCellsPerFrame` | `8192` | `tuning::lighting::kMaxLightingCellsPerFrame` |
| `lightingMaxEventsPerFrame` | `256` | `tuning::lighting::kMaxLightingEventsPerFrame` |

The `DecompositionManager::tick` decomposition budgets are the function's default
arguments (read from `engineConfig()` when omitted); a caller may still pass explicit
per-call values that override the runtime config for that tick.

### Per-material properties (`include/plugin_api.h` `MaterialProperties`)

Not a global knob but the per-material configuration plugins fill to define behavior —
a new material is defined entirely by this struct, with no engine code changes:

| Field | Default | Drives |
|-------|---------|--------|
| `density` | `0.0` | Physics mass and load. |
| `structural_strength` | `0.0` | Collapse resistance (`PropagationSystem`, §B physics). |
| `thermal_conductivity` | `0.0` | Heat and fire spread (`ThermalSystem`). |
| `porosity` | `0.0` | Fluid permeability 0–1 (`FluidSystem`). |
| `hardness` | `0.0` | Resistance to removal/destruction. |
| `light_emission` | `0.0` | Emitted block light 0–1 (`LightingSystem`). |
| `palette_index` | `0` | Index into the 256-entry visual palette (.vox compatibility). |

---

## See also

- [`ARCHITECTURE.md`](ARCHITECTURE.md) §11 (persistence/streaming budgets), §16 (audio),
  §17 (field overlays), §18 (gravity policy).
- [`save-format-versioning.md`](save-format-versioning.md) — the `.vxc` save format
  contract (`chunk_size_voxels` / `voxel_size_m` form a save's world identity).
- `src/core/Tuning.h` — the authoritative source for the §B constants and their
  inline rationale.

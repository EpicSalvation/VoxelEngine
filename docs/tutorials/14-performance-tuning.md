# Tutorial 14: Performance Tuning

Profile and tune your game using three configuration surfaces: YAML layer
configs for world shape, compile-time constants in `Tuning.h` for model
parameters, and runtime `EngineConfig` knobs for per-frame work budgets.
Combine these with fog, far-clip, and streaming controls to ship a smooth
experience on a range of hardware.

---

## Prerequisites

- Engine built and running ([Tutorial 01](01-hello-voxel.md))
- A working game or demo with at least one composite layer
- Familiarity with simulation fields ([Tutorial 12](12-simulation-fields.md))
  and structural physics ([Tutorial 13](13-structural-collapse.md)) if you use
  those systems

---

## 1. Three configuration surfaces

The engine separates configuration into three layers, each with a different
scope and edit cycle:

| Surface | When to change | Rebuild needed? |
|---------|---------------|-----------------|
| **YAML (LayerConfig)** | Per project -- world shape, layer stack, streaming radii | No (loaded at startup) |
| **Tuning.h** | Rarely -- model constants that define simulation behavior | Yes (compile-time constexpr) |
| **EngineConfig** | Frequently -- per-frame budgets, quality presets, live tuning | No (runtime, immediate effect) |

The general rule: start with EngineConfig for quick iteration, then lock in
YAML for your world layout, and only touch Tuning.h when you need to change
fundamental simulation behavior.

---

## 2. YAML (LayerConfig) -- world shape

The layer config YAML defines the world's layer stack and is loaded once at
startup. Changing it requires restarting the demo but not recompiling.

```yaml
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 32
    view_distance_chunks: 8
    resident_chunk_budget: 4096
    streaming_volume:
      shape: box
```

### Key performance knobs

| Field | Effect |
|-------|--------|
| `chunk_size_voxels` | Larger chunks = fewer draw calls but coarser streaming granularity. 32 is a good default. |
| `view_distance_chunks` | Load/evict radius. Halving this roughly quarters the resident chunk count. |
| `resident_chunk_budget` | Hard cap on chunks loaded per layer. `0` = unlimited. Farthest-first eviction when over budget. |
| `streaming_volume.shape` | `box` (default), `sphere` (excludes corners for ~21% fewer chunks), `shell` (thin ring for backdrop-only layers). |

### Decoupling decomposition from view distance

For composite layers, you can set `decompose_distance_m` separately from
`view_distance_chunks`. This lets you show coarse silhouettes far out while
only decomposing to fine detail up close:

```yaml
layers:
  - name: blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
    view_distance_chunks: 12    # show silhouettes far out
    decompose_distance_m: 64.0  # only decompose within 64 m
```

Geometry beyond the decompose distance renders as solid macro blocks -- cheap
to draw and visually acceptable when combined with fog (see section 7).

For the complete YAML field catalog, see
[`docs/configuration-guide.md`](../configuration-guide.md).

---

## 3. Tuning.h -- compile-time constants

These `constexpr` values in `Tuning.h` define the simulation model. They
require a rebuild to change, so treat them as project-level decisions rather
than per-session tuning.

| Namespace | Constant | Default | Purpose |
|-----------|----------|---------|---------|
| `tuning::physics` | `kSupportSpanPerStrength` | 5.0 | Voxels of support reach per unit of `structural_strength` |
| `tuning::physics` | `kMaxSupportSpan` | 16 | Absolute cap on support reach |
| `tuning::thermal` | `kAmbientTemperature` | 20.0 | Baseline temperature (degrees) |
| `tuning::thermal` | `kStabilityFactor` | 1/6 | Diffusion stability bound |
| `tuning::fluid` | `kSaturationThreshold` | 1.0 | Fluid amount that triggers a saturation event |
| `tuning::lighting` | `kAmbientBrightness` | 0.05 | Baseline light level everywhere |
| `tuning::lighting` | `kAttenuationPerStep` | 1/15 | Light falloff per voxel step |
| `tuning::ao` | `kVertexFactor[4]` | {0.46, 0.64, 0.82, 1.0} | Per-vertex AO darkening factors |

The AO factors deserve special attention: set all four to `1.0` to disable
ambient occlusion entirely (useful for a flat-shaded art style or to save mesh
vertex bandwidth).

---

## 4. EngineConfig -- runtime budgets

`EngineConfig` is the primary tool for frame-to-frame performance control. All
budgets take effect immediately -- no restart needed.

```cpp
#include "core/EngineConfig.h"

// Decomposition throughput
engineConfig().decompositionLoadPerFrame   = 8;    // default 4
engineConfig().decompositionDecompPerFrame = 128;  // default 64
engineConfig().decompositionApplyPerFrame  = 32;   // default 16

// Streaming residency
engineConfig().streamingHysteresisChunks = 3;      // default 2

// Structural propagation
engineConfig().physicsMaxAggregateRecomputesPerFrame = 128;
engineConfig().physicsMaxStructuralEventsPerFrame    = 512;
engineConfig().physicsMaxSupportFloodNodes           = 8192;

// Simulation fields
engineConfig().thermalMaxCellsPerFrame   = 8192;
engineConfig().fluidMaxCellsPerFrame     = 8192;
engineConfig().fluidMaxEventsPerFrame    = 512;
engineConfig().lightingMaxCellsPerFrame  = 16384;
engineConfig().lightingMaxEventsPerFrame = 512;

// Reset everything to defaults:
resetEngineConfig();
```

### Overflow semantics

When a system exceeds its per-frame budget, the remaining work carries over to
the next frame. Work is deferred, never lost. This means a sudden spike (a dam
breaking, a building collapsing) spreads its cost over multiple frames rather
than causing a single-frame hitch.

### Quality presets

A common pattern is to define named quality levels that set all budgets at
once:

```cpp
void setQualityLow() {
    engineConfig().decompositionLoadPerFrame = 2;
    engineConfig().thermalMaxCellsPerFrame   = 2048;
    engineConfig().fluidMaxCellsPerFrame     = 2048;
}

void setQualityHigh() {
    engineConfig().decompositionLoadPerFrame = 8;
    engineConfig().thermalMaxCellsPerFrame   = 8192;
    engineConfig().fluidMaxCellsPerFrame     = 8192;
}
```

This keeps the quality menu simple for players while giving you fine-grained
control over every subsystem.

### Streaming hysteresis

`streamingHysteresisChunks` prevents load/evict thrashing at the view-distance
boundary. With the default value of 2, a chunk is loaded when the player enters
`view_distance_chunks` but not evicted until the player moves
`view_distance_chunks + 2` chunks away. Increase this if you see chunks
flickering in and out at the edge of the view.

---

## 5. EngineMetrics

Query the engine's internal counters to identify bottlenecks:

```cpp
EngineMetrics metrics = engine.getMetrics();
// Contains: frame time, draw calls, resident chunks per layer,
// decomposition queue depth, voice count, etc.
```

Use `EngineMetrics` to build in-game performance overlays, log per-frame stats
to a file, or drive adaptive quality -- automatically lowering budgets when
frame time exceeds a target.

---

## 6. Logging

The engine's logging system helps diagnose performance problems without
attaching a debugger:

```cpp
#include "core/Logger.h"

Log::setMinLevel(Log::Level::Info);  // Debug, Info, Warn, Error
Log::info("MyGame", "Player spawned at origin");
Log::warn("MyGame", "Chunk budget exceeded");
Log::error("MyGame", "Failed to load plugin");
```

### Category-tagged logging

Use category tags to filter log output by subsystem:

```cpp
static constexpr const char* kLogCat = "terrain";
Log::debug(kLogCat, "Generating chunk at (0,0,0)");
```

### Custom log handler

Route log output to a file, network endpoint, or in-game console:

```cpp
Log::setHandler([](Log::Level lvl, const char* cat, const char* msg) {
    // Write to file, send to network, etc.
});
Log::setHandler(nullptr);  // restore stderr default
```

Set the minimum level to `Debug` during development to see budget overflow
warnings from the simulation systems, then raise it to `Info` or `Warn` for
release.

---

## 7. Fog and far-clip as LOD-hiding tools

Fog and far-clip are not just visual effects -- they are performance tools.
Use them to hide the LOD transition where fully decomposed terrain meets coarse
macro silhouettes.

### Fog

```cpp
#include "renderer/Fog.h"

FogParams fog{};
fog.color   = glm::vec3(0.6f, 0.65f, 0.7f);  // sky-like blue-gray
fog.near_m  = 80.0f;   // fog starts
fog.far_m   = 200.0f;  // fog at full strength
fog.density = 0.8f;    // max strength [0,1]; 0 = no fog

renderer.setFog(fog);
```

The default density of `0` means no fog at all -- byte-identical behavior to
pre-fog engine builds. Set `density > 0` to enable it.

### Match fog color to clear color

For geometry to dissolve seamlessly into the background, match the fog color
to the renderer's clear color:

```cpp
renderer.setClearColor(glm::vec3(0.6f, 0.65f, 0.7f));
```

### Placement strategy

Tune the fog `near_m` distance to sit just inside the `decompose_distance_m`
so that the transition from detailed terrain to coarse silhouettes happens
inside the fog band and is invisible to the player.

### Far clip

```cpp
renderer.setFarClip(500.0f);  // meters
```

Set the far clip beyond the fog's `far_m` so that geometry is fully fogged out
before it gets clipped. If far-clip is closer than the fog far distance, you
will see hard edges where geometry pops out of existence.

---

## 8. Streaming tuning

Fine-tune how the engine loads and evicts chunks around the player:

### Per-layer YAML knobs

| Knob | Purpose |
|------|---------|
| `view_distance_chunks` | Load/evict radius in chunks. |
| `resident_chunk_budget` | Hard cap on resident chunks per layer. `0` = unlimited. Farthest-first eviction when over budget. |
| `decompose_distance_m` | Decouple decomposition from view distance. Show coarse silhouettes far out, decompose to fine grid only up close. |
| `streaming_volume.shape` | `box` (default), `sphere` (excludes corners), `shell` (thin ring for backdrop-only layers). |

### Hysteresis

`engineConfig().streamingHysteresisChunks` (default 2) adds a buffer zone
between the load and evict boundaries. Without hysteresis, a player standing
exactly at the view-distance edge would cause chunks to thrash in and out every
frame.

### Resident chunk budget

When the number of loaded chunks exceeds `resident_chunk_budget`, the engine
evicts the farthest chunks first. This gives you a hard memory ceiling per
layer. Set it based on your target platform's available memory.

---

## 9. Putting it all together

A typical tuning workflow:

1. **Measure** -- enable `Log::Level::Debug`, run your game, and collect
   `EngineMetrics` each frame. Identify which system is blowing its budget.
2. **Adjust budgets** -- raise the bottleneck system's EngineConfig budget or
   lower competing systems. Verify frame time improves.
3. **Tune streaming** -- reduce `view_distance_chunks` or add a
   `resident_chunk_budget` if memory is the constraint.
4. **Add fog** -- hide the LOD seam with fog. Match the fog band to your
   `decompose_distance_m`.
5. **Lock in YAML** -- once you have found good streaming parameters, commit
   them to your layer config YAML.
6. **Profile deeply** -- for detailed frame-level profiling data, see
   [`docs/m17-performance-profiling.md`](../m17-performance-profiling.md).

---

## Challenge: build an adaptive quality controller

Put all three configuration surfaces to work in one loop.

1. Implement the `setQualityLow` / `setQualityHigh` presets from section 4 and
   bind a key to toggle them manually.
2. Each frame, read `engine.getMetrics()` frame time; if it exceeds your target
   for several consecutive frames, drop to the low preset automatically, and
   restore high when it recovers.
3. Run `14-flow-and-heat`, break a dam to spike simulation load, and watch the
   controller switch presets -- confirm it via an on-screen `EngineMetrics`
   readout.

<details>
<summary>Stuck? Where to look</summary>

- Demo: `demos/14-flow-and-heat/main.cpp` (and `demos/05-decompose-on-approach`
  for the fog step).
- Presets follow the `setQualityLow` / `setQualityHigh` pattern in section 4;
  read frame time from `engine.getMetrics()` (section 5).
- Fog `near_m` / `decompose_distance_m` pairing is covered in section 7.

</details>

**Going further:** add fog whose `near_m` sits just inside `decompose_distance_m`
in `05-decompose-on-approach` and verify the LOD seam disappears into the fog
band.

---

## How to verify

1. **Budget overflow test:** Run any demo with simulation fields (e.g.,
   `14-flow-and-heat`). Set `fluidMaxCellsPerFrame` to 64 and observe the
   fluid spreading slowly over many frames. Set it to 16384 and watch it
   converge almost instantly. The final result is identical -- only the
   convergence speed changes.

   ```bash
   cmake -B build && cmake --build build
   ./build/14-flow-and-heat
   ```

2. **Fog tuning:** Run `05-decompose-on-approach` and enable fog. Fly outward
   and verify that macro silhouettes dissolve into fog before you notice the
   LOD transition.

   ```bash
   ./build/05-decompose-on-approach
   ```

3. **Streaming residency:** In any multi-layer demo, set
   `resident_chunk_budget` to a low value (e.g., 64) in the YAML and observe
   farthest-first eviction as you move through the world. Chunks behind you
   evict to stay within budget.

4. **Quality presets:** Implement the `setQualityLow` / `setQualityHigh`
   pattern from section 4 in your game and toggle between them at runtime.
   Watch `EngineMetrics` frame time change accordingly.

5. **Logging:** Set `Log::setMinLevel(Log::Level::Debug)` and look for budget
   overflow messages in stderr during heavy simulation activity.

---

## Key references

| What | Where |
|------|-------|
| EngineConfig (all runtime knobs) | [`include/core/EngineConfig.h`](../../include/core/EngineConfig.h) |
| Tuning constants (all compile-time) | `src/core/Tuning.h` |
| LayerConfig (YAML schema) | [`include/core/LayerConfig.h`](../../include/core/LayerConfig.h) |
| EngineMetrics | `include/core/EngineMetrics.h` |
| Logger | [`include/core/Logger.h`](../../include/core/Logger.h) |
| FogParams | [`include/renderer/Fog.h`](../../include/renderer/Fog.h) |
| Configuration guide (full knob catalog) | [`docs/configuration-guide.md`](../configuration-guide.md) |
| Performance profiling data | [`docs/m17-performance-profiling.md`](../m17-performance-profiling.md) |
| Simulation fields (field budgets) | [Tutorial 12](12-simulation-fields.md) |
| Structural collapse (physics budgets) | [Tutorial 13](13-structural-collapse.md) |
| Materials (structural_strength, porosity, conductivity) | [Tutorial 03](03-materials-and-properties.md) |

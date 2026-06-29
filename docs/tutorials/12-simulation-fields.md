# Tutorial 12: Simulation Fields

Add fluid flow, heat propagation, and dynamic lighting to your world using
sparse cellular automata that overlay the voxel grid. Fields are not embedded
in `Voxel` -- they live in separate simulation systems that read and write the
world through events.

---

## Prerequisites

- Engine built and running ([Tutorial 01](01-hello-voxel.md))
- Comfortable writing a plugin ([Tutorial 02](02-your-first-plugin.md))
- Familiar with materials and properties ([Tutorial 03](03-materials-and-properties.md))
- Understanding of the world and layer model (layers, chunks, edits via
  `apply_edit`)

---

## 1. The field overlay model

Simulation fields are **sparse cellular automata layered on top of the voxel
grid**. Each field system (thermal, fluid, lighting) maintains its own sparse
data structure keyed by world-space voxel coordinates. Cells are only allocated
where the field is non-zero, so a lava lake does not cost memory in the frozen
tundra on the far side of the map.

The key design principle: fields are overlays, not data packed into the `Voxel`
struct. A voxel's `MaterialProperties` (porosity, thermal conductivity, light
emission) influence how fields propagate through it, but the field values
themselves -- temperature, fluid amount, brightness -- live in the simulation
systems.

Each system follows the same lifecycle:

1. Create the system, passing the `World` and `PluginManager`.
2. Attach it to the engine via the corresponding setter.
3. Register sources and event callbacks from your plugin.
4. Tick the system each frame.

```cpp
auto thermal  = std::make_unique<sim::ThermalSystem>(world, pluginManager);
auto fluid    = std::make_unique<sim::FluidSystem>(world, pluginManager);
auto lighting = std::make_unique<sim::LightingSystem>(world, pluginManager);

engine.setThermalSystem(thermal.get());
engine.setFluidSystem(fluid.get());
engine.setLightingSystem(lighting.get());

// Per-frame tick (thermal before fluid):
thermal->tick(dt);
fluid->tick(dt);
```

The tick order matters: thermal runs first so that heat-dependent fluid
behavior (e.g., ice melting) sees up-to-date temperatures. The lighting system
is ticked internally by the renderer and does not require an explicit call.

---

## 2. Fluid system

The fluid system simulates liquid flow through the voxel grid. Fluid spreads
outward and downward from source cells, permeating through porous materials and
pooling against impermeable walls.

### Registering a fluid source

A source continuously injects fluid at a given rate:

```cpp
ctx->register_fluid_source(ctx, WorldCoord(10, 5, 10), /*rate=*/0.5f, "water");
```

The `rate` is fluid units per second. The `material_id` string (`"water"`)
determines the palette index used when the event fires.

### Responding to fluid events

When a cell crosses the saturation threshold (fills up) or drains below it, the
system fires a `FluidEvent`. Your plugin decides what to do -- typically placing
or removing a fluid material:

```cpp
ctx->register_on_fluid_event(ctx, [](const FluidEvent* event, void* ud) {
    if (event->crossing == FieldCrossing::Rising) {
        // Voxel saturated — place fluid material
        Voxel fluid;
        fluid.material.palette_index = event->palette_index;
        ctx->apply_edit(ctx, event->position, &fluid);
    } else {  // FieldCrossing::Falling
        // Voxel drained — remove fluid
        Voxel empty = Voxel::empty();
        ctx->apply_edit(ctx, event->position, &empty);
    }
}, nullptr);
```

### FluidEvent struct

```cpp
struct FluidEvent {
    WorldCoord position;
    int64_t voxel_x, voxel_y, voxel_z;
    float amount;
    FieldCrossing crossing;  // Rising or Falling
    const char* material_id;
    uint8_t palette_index;
};
```

`FieldCrossing::Rising` means the cell just reached saturation.
`FieldCrossing::Falling` means the cell just drained below the threshold.

### Porosity-driven permeation

The `porosity` field in `MaterialProperties` controls how fluid moves through a
material. High porosity (close to 1.0) lets fluid seep through freely -- think
sand or gravel. Low porosity (close to 0.0) blocks flow -- think granite or
glass. A porosity of exactly 0.0 makes the material fully impermeable; fluid
pools against it.

This means terrain composition directly shapes fluid behavior without any
special-case code. Build a wall of stone and water stops. Build a sand dam and
it slowly leaks through.

---

## 3. Thermal system

The thermal system simulates heat diffusion through the voxel grid. Heat
spreads from sources through surrounding voxels at a rate governed by each
material's thermal conductivity.

### Registering a heat source

```cpp
ctx->register_heat_source(ctx, WorldCoord(5, 3, 5), /*rate=*/100.0f);
```

The `rate` is temperature units per second emitted by the source.

### Responding to thermal events

When a cell's temperature crosses a threshold, the system fires a
`ThermalFieldEvent`:

```cpp
ctx->register_on_thermal_event(ctx, [](const ThermalFieldEvent* event, void* ud) {
    if (event->crossing == FieldCrossing::Rising) {
        // Temperature threshold crossed — voxel is hot
    }
}, nullptr);
```

### ThermalFieldEvent struct

```cpp
struct ThermalFieldEvent {
    WorldCoord position;
    int64_t voxel_x, voxel_y, voxel_z;
    float temperature;
    FieldCrossing crossing;
};
```

### Conductivity-driven diffusion

Heat spreads faster through materials with high `thermal_conductivity`. Metal
conducts heat quickly; wood conducts slowly; air barely at all. The diffusion
solver uses a stability factor of `1/6` (see
[`docs/configuration-guide.md`](../configuration-guide.md) for all tuning
constants) to keep the simulation stable at any frame rate.

---

## 4. Lighting system

The lighting system handles both sky light and block-emitter light. It
propagates brightness through the voxel grid, attenuating per step, and
computes ambient occlusion at mesh vertices.

### Registering a point light

```cpp
ctx->register_light_source(ctx, WorldCoord(8, 6, 8), /*brightness=*/0.9f);
```

### Light emission via material

Instead of placing explicit light sources, you can make a material glow by
setting `light_emission` in its `MaterialProperties` (range 0.0 to 1.0). Every
voxel of that material acts as a light source at the given brightness. This is
the natural way to make lava, glowstone, or lantern blocks illuminate their
surroundings.

### Responding to lighting events

```cpp
ctx->register_on_lighting_event(ctx, [](const LightingEvent* event, void* ud) {
    // event->brightness, event->crossing
}, nullptr);
```

### LightingEvent struct

```cpp
struct LightingEvent {
    WorldCoord position;
    int64_t voxel_x, voxel_y, voxel_z;
    float brightness;
    FieldCrossing crossing;
};
```

### Ambient occlusion

The mesh builder bakes per-vertex ambient occlusion into the mesh. Each vertex
samples its four neighboring voxel corners and applies a darkening factor:

```cpp
// tuning::ao::kVertexFactor[4] = {0.46, 0.64, 0.82, 1.0}
```

The four values correspond to 0, 1, 2, or 3 open (non-occluding) neighbors.
Corners tucked into concavities get the darkest factor (0.46); fully exposed
corners get 1.0 (no darkening). To disable AO entirely, set all four values to
1.0.

---

## 5. Field readback

You can query the current field value at any world position without waiting for
an event:

```cpp
float temperature = engine.temperatureAt(position);
float fluidAmount = engine.fluidAmountAt(position);
float light       = engine.lightAt(position);
```

This is useful for HUD displays (show the temperature under the crosshair),
gameplay logic (damage the player if temperature exceeds a threshold), or debug
overlays.

---

## 6. Per-frame budgets via EngineConfig

Each field system processes a bounded number of cells per frame to keep the
simulation from stalling the render loop. The budget is configurable at runtime
through `EngineConfig`:

```cpp
#include "core/EngineConfig.h"

engineConfig().thermalMaxCellsPerFrame   = 8192;   // default 4096
engineConfig().fluidMaxCellsPerFrame     = 8192;
engineConfig().fluidMaxEventsPerFrame    = 512;    // default 256
engineConfig().lightingMaxCellsPerFrame  = 16384;  // default 8192
engineConfig().lightingMaxEventsPerFrame = 512;
```

When a tick exceeds its budget, the remaining work carries over to the next
frame. Work is never lost, just deferred. This means a sudden flood of activity
(breaking a dam, igniting a forest) spreads the cost over multiple frames rather
than causing a single-frame hitch.

For a complete guide to budget tuning across all engine systems, see
[Tutorial 14: Performance Tuning](14-performance-tuning.md).

---

## 7. Translucent palette for fluid rendering

Fluid materials like water and glass look best with translucent palette colors.
Set the alpha channel below `0xff` to enable translucency:

```cpp
ctx->set_palette_color(ctx, 10, 0x50f0e8d8);  // alpha 0x50 = translucent
```

The renderer sorts translucent faces back-to-front automatically. See
[Tutorial 03](03-materials-and-properties.md) for more on palette colors.

---

## Challenge: let material properties shape a flow

See `porosity` drive behavior with no special-case code.

1. Register a fluid source and build two walls in front of it -- one of an
   impermeable material (`porosity = 0`) and one of a porous material
   (`porosity ~= 0.6`).
2. Run `14-flow-and-heat` and watch fluid pool against the solid wall but slowly
   seep through the porous one.
3. Add a heat source near the fluid and use the `temperatureAt` / `fluidAmountAt`
   readback (section 5), or the HUD demo, to watch the values change in real
   time.

<details>
<summary>Stuck? Where to look</summary>

- Demo: `demos/14-flow-and-heat/main.cpp`.
- `register_fluid_source` / `register_heat_source` are in sections 2-3;
  `porosity` is a `MaterialProperties` field (section 2).
- Read values back with `engine.fluidAmountAt` / `temperatureAt` (section 5);
  budgets live in `EngineConfig` (section 6).

</details>

**Going further:** lower `fluidMaxCellsPerFrame` to 256 and confirm the flow
reaches the same equilibrium, just over more frames -- budgeted work is
deferred, never lost.

---

## How to verify

1. **Build and run the flow-and-heat demo:**

   ```bash
   cmake -B build && cmake --build build
   ./build/14-flow-and-heat
   ```

   You should see fluid spreading outward from source points, pooling against
   impermeable walls, and seeping through porous materials. Heat sources glow
   and warm nearby voxels. Walk near a heat source and observe the thermal
   event threshold being crossed.

2. **Build and run the HUD demo for field readback:**

   ```bash
   ./build/18-hud-and-controls
   ```

   The HUD displays live temperature, fluid amount, and light level under the
   crosshair. Walk toward a fluid source or heat source and watch the values
   change in real time.

3. **Experiment with budgets:** Lower `fluidMaxCellsPerFrame` to 512 and
   observe that fluid still spreads to the same extent -- it just takes more
   frames to converge. Raise it back to 8192 and watch the simulation snap to
   equilibrium faster.

---

## Key references

| What | Where |
|------|-------|
| FluidEvent, ThermalFieldEvent, LightingEvent structs | [`include/plugin_api.h`](../../include/plugin_api.h) |
| FieldCrossing enum | [`include/plugin_api.h`](../../include/plugin_api.h) |
| MaterialProperties (porosity, thermal_conductivity, light_emission) | [`include/plugin_api.h`](../../include/plugin_api.h) |
| EngineConfig (per-frame budgets) | [`include/core/EngineConfig.h`](../../include/core/EngineConfig.h) |
| Tuning constants (AO, thermal, fluid, lighting) | `src/core/Tuning.h` |
| Flow-and-heat demo | `demos/14-flow-and-heat/main.cpp` |
| HUD demo (field readback) | `demos/18-hud-and-controls/main.cpp` |
| Architecture: simulation fields | [`docs/architecture.md`](../architecture.md) |
| Configuration guide | [`docs/configuration-guide.md`](../configuration-guide.md) |
| Performance tuning | [Tutorial 14](14-performance-tuning.md) |

# Tutorial 02: Your First Plugin

Write a plugin that registers a custom material and a terrain generator, load
it into a demo, and see the results on screen.

---

## Prerequisites

- The engine builds and runs successfully (see
  [Tutorial 01](01-hello-voxel.md))
- Familiarity with the engine lifecycle (`Engine`, `LayerConfig`,
  `PluginManager`, game loop)
- A C++17 compiler

---

## 1. The plugin contract

A plugin is a single `plugin.cpp` file placed under `plugins/<name>/`. The
build system discovers it automatically -- no CMake edits are required. It is
compiled into a shared library (`.so` / `.dylib` / `.dll`) and loaded at
runtime by `PluginManager`.

Every plugin must:

1. Include `plugin_api.h`.
2. Export a C-linkage function named `voxel_plugin_init`.
3. Stamp its ABI version with `VOXEL_PLUGIN_ABI_STAMP()`.
4. Return `0` from init on success (non-zero aborts the load).

The current ABI version is **2** (`VOXEL_PLUGIN_ABI_VERSION = 2`). The engine
checks the stamp at load time and rejects a mismatch with a diagnostic message
rather than risking silent memory corruption.

---

## 2. Minimal plugin skeleton

Create `plugins/my-terrain/plugin.cpp`:

```cpp
#include "plugin_api.h"
#include "world/Voxel.h"

VOXEL_PLUGIN_ABI_STAMP();

extern "C" VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // Register materials, generators, recipes here.
    return 0;  // 0 = success
}
```

`VOXEL_PLUGIN_EXPORT` is defined in `plugin_api.h` and resolves to
`__declspec(dllexport)` on Windows or nothing on other platforms (C linkage
provides symbol visibility on Linux/macOS).

A plugin links **zero engine symbols**. It talks to the engine exclusively
through the `PluginContext` function pointers and the inline helpers in
`plugin_api.h`.

---

## 3. Register a material

A material is a `MaterialProperties` struct -- a bag of physical values that
simulation systems read directly. There is no block-type enum anywhere in the
engine.

```cpp
MaterialProperties stone{};
stone.density              = 2600.0f;   // kg/m^3 -- physics mass
stone.hardness             = 0.8f;      // removal resistance
stone.structural_strength  = 0.9f;      // collapse resistance
stone.palette_index        = 1;         // visual color index

ctx->register_material(ctx, "stone", stone);
ctx->set_palette_color(ctx, 1, 0xff808080);  // ABGR: opaque gray
```

### MaterialProperties fields

| Field | Type | Description |
|-------|------|-------------|
| `density` | float | kg/m^3; drives physics mass and structural load |
| `structural_strength` | float | Collapse resistance (PropagationSystem) |
| `thermal_conductivity` | float | Heat spread rate (ThermalSystem) |
| `porosity` | float | Fluid permeability [0,1] (FluidSystem) |
| `hardness` | float | Removal resistance (RemovalAccumulator) |
| `light_emission` | float | [0,1] emitted block light (LightingSystem) |
| `palette_index` | uint8_t | Index into the 256-entry visual palette |

For the full property-to-system mapping, see
[Tutorial 03](03-materials-and-properties.md).

### Palette colors: ABGR format

Colors are packed as `0xAABBGGRR`. Alpha `0xff` is opaque; alpha below `0xff`
makes the material translucent (routed into the alpha render pass -- this is
how water and glass are drawn).

---

## 4. Write a layer generator

A `LayerGeneratorFn` fills a chunk's voxel grid from scratch. It receives the
chunk's world-space origin, the grid size (voxels per side), and a flat output
array of `grid_size^3` voxels in row-major order (x fastest).

```cpp
void my_terrain(WorldCoord chunk_origin, int grid_size,
                Voxel* out_voxels, void* user_data) {
    for (int z = 0; z < grid_size; ++z)
        for (int y = 0; y < grid_size; ++y)
            for (int x = 0; x < grid_size; ++x) {
                int idx = x + grid_size * (y + grid_size * z);
                double wy = chunk_origin.value.y + y;
                if (wy < 4.0)
                    out_voxels[idx].material = stone;
                else
                    out_voxels[idx] = Voxel::empty();
            }
}
```

This generator creates a flat stone floor: every voxel below world-y 4.0 is
stone; everything above is empty (air).

### The determinism rule

Generators must be **pure functions of (position, seed)**. No `rand()`, no
`time()`, no global mutable state. Streamed chunks regenerate on demand -- if
the generator is not deterministic, terrain will tear at chunk seams or change
on reload.

For deterministic randomness, use the header-only RNG helpers in
`plugin_api.h`:

```cpp
uint64_t voxel_rng_next(uint64_t* state);       // splitmix64
float    voxel_rng_norm(uint64_t* state);        // uniform [0,1)
uint64_t voxel_seed_mix(uint64_t a, uint64_t b); // fold two ints into a seed
```

### Voxel indexing

The flat array uses `x + grid_size * (y + grid_size * z)` -- x is the fastest
axis, z the slowest. This layout matches every demo, plugin, and the engine's
internal chunk storage.

---

## 5. Register the generator

Back in `voxel_plugin_init`, register the generator for the layer named
`"terrain"`:

```cpp
ctx->register_layer_generator(ctx, "terrain", my_terrain, nullptr);
```

The fourth argument is `user_data` -- an opaque pointer the engine passes back
to every call of `my_terrain`. Pass `nullptr` when you do not need it, or use
it to thread configuration (voxel sizes, noise seeds, etc.) into the generator
without global state.

---

## 6. Complete plugin

Here is `plugins/my-terrain/plugin.cpp` in full:

```cpp
#include "plugin_api.h"
#include "world/Voxel.h"

VOXEL_PLUGIN_ABI_STAMP();

namespace {

MaterialProperties g_stone{};

void my_terrain(WorldCoord chunk_origin, int grid_size,
                Voxel* out_voxels, void* /*user_data*/) {
    for (int z = 0; z < grid_size; ++z)
        for (int y = 0; y < grid_size; ++y)
            for (int x = 0; x < grid_size; ++x) {
                int idx = x + grid_size * (y + grid_size * z);
                double wy = chunk_origin.value.y + y;
                if (wy < 4.0)
                    out_voxels[idx].material = g_stone;
                else
                    out_voxels[idx] = Voxel::empty();
            }
}

}  // namespace

extern "C" VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // Material.
    g_stone.density             = 2600.0f;
    g_stone.hardness            = 0.8f;
    g_stone.structural_strength = 0.9f;
    g_stone.palette_index       = 1;

    ctx->register_material(ctx, "stone", g_stone);
    ctx->set_palette_color(ctx, 1, 0xff808080);  // opaque gray (ABGR)

    // Generator.
    ctx->register_layer_generator(ctx, "terrain", my_terrain, nullptr);

    return 0;
}
```

---

## 7. Build

No CMake edits are needed. Rebuild:

```bash
cmake -B build
cmake --build build
```

The build system discovers `plugins/my-terrain/plugin.cpp` automatically and
produces `build/plugins/my-terrain.so` (or `.dylib` / `.dll`).

---

## 8. Load the plugin from a demo

### Option A: Load from disk at runtime

```cpp
PluginManager pluginManager;
auto id = pluginManager.loadPlugin(VOXEL_PLUGIN_PATH);  // returns PluginId
```

`loadPlugin` opens the shared library, reads the ABI stamp, and calls
`voxel_plugin_init`. If the ABI version does not match, the load aborts with a
diagnostic. `VOXEL_PLUGIN_PATH` is typically defined at build time by CMake.

### Option B: Wire in a compiled-in plugin

For tests or demos that compile the plugin source directly:

```cpp
pluginManager.wireInPlugin(voxel_plugin_init);
```

This calls `voxel_plugin_init` directly without loading a shared library.
No ABI version check is performed (compiled-in code is always in sync).

### Retrieve the generator for chunk-streamed worlds

When streaming terrain in a demo, you retrieve the registered generator by
layer name:

```cpp
LayerGeneratorFn generator = nullptr;
void* generatorUserData = nullptr;
for (const auto& g : pluginManager.layerGenerators()) {
    if (g.layer_name == "terrain") {
        generator = g.fn;
        generatorUserData = g.user_data;
    }
}
```

See `demos/03-plugin-driven-world/main.cpp` for the full streaming pattern
that uses this generator to fill chunks on demand.

---

## Challenge: add a second material

Reinforce the material + generator pattern by giving `my-terrain` a soil cap on
top of its stone floor. Try it before expanding the solution.

1. Register a second material (`soil`) with a distinct `palette_index` and
   palette color, just like `stone` in section 3.
2. In `my_terrain`, place soil in the top layer of the floor and stone below.
3. Rebuild (no CMake edits needed) and run. You should see a soil-colored
   surface over a stone base.

<details>
<summary>Show solution (the generator branch)</summary>

```cpp
if      (wy < 3.0) out_voxels[idx].material = g_stone;
else if (wy < 4.0) out_voxels[idx].material = g_soil;
else               out_voxels[idx] = Voxel::empty();
```

</details>

**Going further:** vary the surface height with x/z by deriving it from
`voxel_seed_mix` + `voxel_rng_norm` seeded by world position. Keep it a pure
function of position so streamed chunks stay seamless -- that's the determinism
rule from section 4 in action.

---

## How to verify

1. **Build and run:**

   ```bash
   cmake -B build && cmake --build build
   ./build/03-plugin-driven-world
   ```

   You should see a flat terrain surface generated by the `base-terrain` plugin.
   Press **F** to enter free-camera mode and fly over the landscape.

2. **Runtime plugin toggling:** In demo 03, press **P** to load/unload the
   water plugin at runtime. When water is unloaded, the world is exactly the
   base-terrain heightmap. When loaded, flat blue water floods every empty
   voxel up to a fixed sea level. This demonstrates that plugins are fully
   hot-swappable.

3. **Test your own plugin:** Replace `VOXEL_BASE_PLUGIN_PATH` with the path to
   your `my-terrain.so` or wire it in with `wireInPlugin`. You should see a
   flat gray stone floor 4 voxels high.

---

## Key references

| What | Where |
|------|-------|
| Plugin API (all registration points) | `include/plugin_api.h` |
| ABI version and stamp | `include/plugin_api.h` (`VOXEL_PLUGIN_ABI_VERSION`, `VOXEL_PLUGIN_ABI_STAMP`) |
| Example compiled-in plugin | `src/plugins/ExamplePlugin.cpp` |
| Base-terrain plugin (disk-loaded) | `plugins/base-terrain/plugin.cpp` |
| Plugin-driven demo | `demos/03-plugin-driven-world/main.cpp` |
| Material system design | [`docs/architecture.md`](../architecture.md) section 5 |
| Plugin system design | [`docs/architecture.md`](../architecture.md) section 8 |
| Material deep dive | [Tutorial 03](03-materials-and-properties.md) |
| Recipe-driven composition | [Tutorial 04](04-composition-recipes.md) |

# Tutorial 03: Materials and Properties

Define materials whose physical properties drive every simulation system in the
engine -- physics mass, mining time, structural collapse, fluid flow, heat
spread, and lighting -- all without touching engine code.

---

## Prerequisites

- The engine builds and runs successfully (see
  [Tutorial 01](01-hello-voxel.md))
- You know how to write and register a plugin (see
  [Tutorial 02](02-your-first-plugin.md))
- Familiarity with `MaterialProperties`, `register_material`, and
  `set_palette_color`

---

## 1. The material-driven philosophy

Voxels in this engine are **not** block-type IDs. There is no
`enum BlockType { Stone, Dirt, Water, ... }`. Instead, every voxel carries a
`MaterialProperties` struct -- a bag of physical values. Simulation systems
read these values and respond accordingly:

- The **PhysicsSystem** reads `density` to compute mass.
- The **RemovalAccumulator** reads `hardness` to determine mining time.
- The **PropagationSystem** reads `structural_strength` to decide whether a
  structure collapses.
- The **FluidSystem** reads `porosity` to decide whether fluid seeps through.
- The **ThermalSystem** reads `thermal_conductivity` to spread heat.
- The **LightingSystem** reads `light_emission` to emit block light.
- The **Renderer** reads `palette_index` to look up the visual color.

To create a new material -- volcanic glass, packed ice, steel alloy -- you
fill a `MaterialProperties` struct and register it. No engine system needs to
be modified. The simulation reacts to the properties you assign.

For the full design rationale, see
[`docs/architecture.md`](../architecture.md) section 5.

---

## 2. MaterialProperties field reference

Every field, its type, its default, and the engine system that consumes it:

| Field | Type | Default | Consumer system |
|-------|------|---------|-----------------|
| `density` | float | 0.0f | PhysicsSystem (mass, structural load) |
| `hardness` | float | 0.0f | RemovalAccumulator (mining time) |
| `structural_strength` | float | 0.0f | PropagationSystem (collapse resistance) |
| `porosity` | float | 0.0f | FluidSystem (fluid permeability, range [0,1]) |
| `thermal_conductivity` | float | 0.0f | ThermalSystem (heat spread rate) |
| `light_emission` | float | 0.0f | LightingSystem (block light, range [0,1]) |
| `palette_index` | uint8_t | 0 | Renderer (visual color lookup) |

### Special values

- **`density = 0`** makes a voxel empty (`Voxel::isEmpty()` returns true). The
  renderer skips it entirely.
- **`hardness = -1.0f`** makes a material indestructible. The
  `RemovalAccumulator` refuses to accumulate removal work against it, so the
  voxel can never be mined. This is how bedrock works.
- **`light_emission > 0`** causes the LightingSystem to treat the voxel as a
  light source. The value scales the emitted brightness.

---

## 3. Palette colors: the ABGR format

The engine's 256-entry palette stores colors in **ABGR** format, packed into a
`uint32_t`: `0xAABBGGRR`.

| Component | Byte position | Example (opaque gray) |
|-----------|---------------|-----------------------|
| Alpha | `AA` | `0xff` (fully opaque) |
| Blue | `BB` | `0x80` |
| Green | `GG` | `0x80` |
| Red | `RR` | `0x80` |

Set a palette color from a plugin:

```cpp
ctx->set_palette_color(ctx, 1, 0xff808080);  // opaque gray at index 1
```

### Translucency

Set alpha below `0xff` to make a material translucent. The mesh builder skips
per-face brightness shading on translucent voxels and draws them in a separate
blended render pass. This is how water and glass are rendered:

```cpp
ctx->set_palette_color(ctx, 10, 0x50f0e8d8);  // alpha 0x50 = translucent
```

Palette index 0 is reserved for empty voxels and is never rendered.

---

## 4. Multi-material strata example

The `material-showcase` plugin demonstrates a layered terrain where each
stratum has distinct physical properties. Here is the registration pattern:

```cpp
// Topsoil: light, weak, porous, fast to mine.
MaterialProperties topsoil{};
topsoil.density             = 1200.0f;
topsoil.hardness            = 0.2f;
topsoil.structural_strength = 0.1f;
topsoil.porosity            = 0.5f;
topsoil.palette_index       = 2;

ctx->register_material(ctx, "topsoil", topsoil);
ctx->set_palette_color(ctx, 2, 0xff2d6b30);  // opaque brown-green

// Stone: medium density, hard, strong, impermeable.
MaterialProperties stone{};
stone.density             = 2600.0f;
stone.hardness            = 0.6f;
stone.structural_strength = 0.9f;
stone.porosity            = 0.0f;
stone.palette_index       = 3;

ctx->register_material(ctx, "stone", stone);
ctx->set_palette_color(ctx, 3, 0xff808080);  // opaque gray

// Iron ore: very dense, very hard, slight light emission.
MaterialProperties iron{};
iron.density             = 7800.0f;
iron.hardness            = 0.9f;
iron.structural_strength = 0.95f;
iron.light_emission      = 0.05f;
iron.palette_index       = 4;

ctx->register_material(ctx, "iron", iron);
ctx->set_palette_color(ctx, 4, 0xff404880);  // dark reddish-brown

// Bedrock: indestructible floor.
MaterialProperties bedrock{};
bedrock.density             = 3000.0f;
bedrock.hardness            = -1.0f;  // indestructible
bedrock.structural_strength = 1.0f;
bedrock.palette_index       = 5;

ctx->register_material(ctx, "bedrock", bedrock);
ctx->set_palette_color(ctx, 5, 0xff303030);  // dark gray
```

### Placing strata in the generator

A layer generator places these materials by world-y height:

```cpp
void strata_terrain(WorldCoord chunk_origin, int n,
                    Voxel* out, void* /*user_data*/) {
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                int idx = x + n * (y + n * z);
                double wy = chunk_origin.value.y + y;

                if      (wy < 0.0) out[idx].material = bedrock;
                else if (wy < 6.0) out[idx].material = stone;
                else if (wy < 8.0) out[idx].material = topsoil;
                else               out[idx] = Voxel::empty();
            }
}
```

The result: dig down through soft topsoil (clears quickly), hit stone (takes
longer), then iron ore (very slow), and finally bedrock (impossible). Each
stratum's behavior emerges purely from its property values.

---

## 5. Runtime material lookup

Registered materials can be looked up by name at runtime:

```cpp
MaterialProperties mat = pluginManager.material("stone");
```

This is useful in demos and tools that need to inspect or display material
properties (the HUD in `08-material-matters` uses this to show the targeted
voxel's hardness and density).

---

## 6. Material registration in context

Materials are registered through the `PluginContext` during plugin init:

```cpp
ctx->register_material(ctx, "stone", stone);
```

The string ID (`"stone"`) is how recipes, feature generators, and tooling
refer to the material. The engine deep-copies the `MaterialProperties` value.

Palette colors are set separately:

```cpp
ctx->set_palette_color(ctx, 3, 0xff808080);
```

This two-step pattern (register the material, then set its palette color)
keeps the material's physical identity separate from its visual appearance.
The same material could have different visual representations in different
contexts.

For the complete recipe-book reference on creating voxels (including composite
blocks with boundary overrides), see
[`docs/creating-voxels.md`](../creating-voxels.md).

---

## Challenge: add your own stratum

Feel how properties alone drive behavior by adding a new material to the strata.

1. Register a new material -- say `glowstone` -- with a high `light_emission`, a
   moderate `hardness`, and a bright palette color.
2. Insert a band of it into `strata_terrain` between stone and topsoil.
3. Rebuild and run `08-material-matters`. Mine down to your band: the HUD shows
   its `hardness`/`density`, the wireframe ramp reflects the mining time, and
   (with the lighting system active) it glows -- all from the property values
   you set, with no engine changes.

<details>
<summary>Stuck? Where to look</summary>

- Reference plugin: `plugins/material-showcase/plugin.cpp`, plus the strata
  registration in section 4.
- Add your band to the height test in `strata_terrain`.
- `light_emission` and `hardness` are `MaterialProperties` fields (section 2);
  `hardness = -1.0f` is the indestructible sentinel.

</details>

**Going further:** set `hardness = -1.0f` on one material and confirm the
`RemovalAccumulator` refuses to break it. You've just made bedrock from a
property value alone.

---

## How to verify

1. **Build and run the material-matters demo:**

   ```bash
   cmake -B build && cmake --build build
   ./build/08-material-matters
   ```

2. **Observe material-driven behavior:**
   - Aim at different strata and hold left mouse to mine. Topsoil clears
     almost instantly. Stone takes noticeably longer. Iron ore is very slow.
     Bedrock never clears at all.
   - The HUD displays the targeted voxel's `hardness`, `density`, and
     `structural_strength` -- all read directly from its `MaterialProperties`.
   - The wireframe highlight around the targeted voxel ramps toward red as
     removal work accrues, giving visual feedback of the hardness difference.

3. **Place different materials:** Press **1** through **6** to select a
   material, then right-click to place it. Place a diamond block and try to
   mine it back -- it is hard but removable. Place bedrock -- it is
   indestructible.

---

## Key references

| What | Where |
|------|-------|
| MaterialProperties struct | `include/plugin_api.h` |
| Voxel definition | `src/world/Voxel.h` |
| Palette and default colors | `src/renderer/Palette.h` |
| Material-showcase plugin (strata) | `plugins/material-showcase/plugin.cpp` |
| Material-matters demo | `demos/08-material-matters/main.cpp` |
| Material system design | [`docs/architecture.md`](../architecture.md) section 5 |
| Creating voxels recipe book | [`docs/creating-voxels.md`](../creating-voxels.md) |
| Plugin registration | [Tutorial 02](02-your-first-plugin.md) |
| Composition recipes | [Tutorial 04](04-composition-recipes.md) |

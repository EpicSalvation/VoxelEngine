# Tutorial: Creating Interesting Voxels

This tutorial shows how to define your own voxels for the engine — from a simple
new colored material to a Minecraft-style "grass block" with a grass top and
dirt sides. It is written against the real plugin API in
[`include/plugin_api.h`](../include/plugin_api.h); every snippet uses APIs the
engine actually exposes.

> **Read first:** [`docs/architecture.md`](architecture.md) §5 (materials),
> §6 (composition recipes), and the rendering notes below. The mental model
> matters here — voxels in this engine are *not* block-type IDs.

---

## 1. How voxels look: the mental model

Two facts drive everything in this tutorial.

**A voxel is a bag of material properties, not a block type.** Every voxel
carries a [`MaterialProperties`](../include/plugin_api.h) record — density,
hardness, thermal conductivity, porosity, structural strength — plus a single
`palette_index` (0–255). There is no `enum BlockType`. Simulation systems read
the *properties*; the renderer reads the *palette index*. See
[`src/world/Voxel.h`](../src/world/Voxel.h):

```cpp
struct Voxel {
    MaterialProperties material;          // the whole identity of the voxel
    static Voxel empty();                 // density 0, palette_index 0 — not rendered
    bool isEmpty() const;
};
```

**A voxel renders as one solid color, brightness-shaded per face.** The mesh
builder ([`src/renderer/ChunkMeshData.cpp`](../src/renderer/ChunkMeshData.cpp))
looks up `palette::color(palette_index)` for the voxel and emits its visible
faces. Each of the six face directions has a fixed brightness multiplier, so a
single voxel already reads as a 3D cube (top brightest, bottom darkest):

```cpp
constexpr Face kFaces[6] = {
    {  0,  0,  1, /*+Z front */ 0.8f },
    {  0,  0, -1, /*-Z back  */ 0.8f },
    {  0, -1,  0, /*-Y bottom*/ 0.5f },
    {  0,  1,  0, /*+Y top   */ 1.0f },
    { -1,  0,  0, /*-X left  */ 0.6f },
    {  1,  0,  0, /*+X right */ 0.6f },
};
```

The vertex format carries only **position + packed ABGR color** — no UVs, no
texture sampler:

```cpp
struct MeshVertex { float x, y, z; uint32_t abgr; };
```

**What this means for you:** a single terminal voxel cannot, today, show a
*different texture on each face* (green grass on top, brown dirt on the sides of
the same cube). The engine has no texture atlas and the shader does no UV
sampling — see [§5](#5-going-further-true-per-face-textures). But you have two
powerful tools that get you most of the "interesting voxel" experience:

1. **Custom materials with custom palette colors** ([§3](#3-recipe-a-new-colored-material)).
2. **Composite blocks with boundary-override recipes** — the engine-native way
   to build a grass-top/dirt-side block ([§4](#4-recipe-a-minecraft-style-grass-block)).

Everything you register lives in a **plugin**: a shared library exporting
`voxel_plugin_init`. Engine code never changes.

---

## 2. The shape of a voxel plugin

Every plugin is one `plugin.cpp` under `plugins/<name>/`. The build discovers it
automatically (no CMake edits — see [README](../README.md) "Setup"). The minimum
skeleton:

```cpp
#include "plugin_api.h"
#include "world/Voxel.h"

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // register materials, palette colors, layer generators, recipes here…
    return 0;  // non-zero aborts the load
}
```

You call `ctx->register_*` function pointers to add things. A plugin links
**zero engine symbols** (it talks only through `ctx` and the inline helpers in
`plugin_api.h`), which is why every example re-implements its own noise instead
of calling into `src/`.

---

## 3. Recipe: a new colored material

This is the simplest "new voxel." Define the material's physical properties,
pick a palette slot, and (optionally) install a color for that slot so it
doesn't inherit a default cycling color.

```cpp
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // 1. Choose a palette slot (0 is reserved for "empty"). Slots 1-15 already
    //    have meaningful default colors (see src/renderer/Palette.h); pick an
    //    unused index, or reuse one whose default color already suits you.
    constexpr uint8_t kMossIdx = 32;

    // 2. Install an ABGR color (0xAABBGGRR). Alpha 0xff = opaque; alpha < 0xff
    //    marks the material TRANSLUCENT and routes its faces into the alpha
    //    pass (this is how water is drawn). Here: an opaque mossy green.
    ctx->set_palette_color(ctx, kMossIdx, 0xff2f8f3f);

    // 3. Define the material's physical identity. These values drive physics,
    //    fire spread, fluid permeability, mining time — not just looks.
    MaterialProperties moss{};
    moss.density              = 900.0f;   // light
    moss.structural_strength  = 0.2f;     // weak; collapses easily
    moss.thermal_conductivity = 0.4f;
    moss.porosity             = 0.6f;     // soaks up fluid
    moss.hardness             = 0.15f;    // fast to mine
    moss.palette_index        = kMossIdx;

    // 4. Register it by id so tooling (build menu, HUD, recipes) can find it.
    ctx->register_material(ctx, "moss", moss);

    return 0;
}
```

### Color format cheat-sheet

The palette is **ABGR** packed into a `uint32_t`: `0xAABBGGRR`.

| Component | Byte | Example (mossy green) |
|-----------|------|-----------------------|
| Alpha     | `AA` | `0xff` (opaque)       |
| Blue      | `BB` | `0x3f`                |
| Green     | `GG` | `0x8f`                |
| Red       | `RR` | `0x2f`                |

Set **alpha below `0xff`** to make a translucent material (glass, water, ice).
The mesh builder skips per-face brightness shading on translucent voxels and
draws them in a separate blended pass.

### Placing the material in the world

A material that nothing places never appears. Register a **layer generator** that
fills a chunk's voxel grid. This one drops a layer of moss on the surface (adapt
the heightmap logic from
[`ExamplePlugin.cpp`](../src/plugins/ExamplePlugin.cpp)):

```cpp
static MaterialProperties g_moss, g_stone;   // captured at init for the generator

static void moss_terrain(WorldCoord chunk_origin, int n, Voxel* out, void*) {
    const int64_t baseY = static_cast<int64_t>(std::llround(chunk_origin.value.y));
    // … compute a surface height per (x,z) column …
    for (int z = 0; z < n; ++z)
      for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            Voxel& v = out[x + n * (y + n * z)];   // row-major, x fastest
            int64_t worldY = baseY + y;
            if      (worldY >  surface) v = Voxel::empty();
            else if (worldY == surface) v.material = g_moss;   // surface skin
            else                        v.material = g_stone;  // interior
        }
}
// … ctx->register_layer_generator(ctx, "terrain", moss_terrain, nullptr);
```

> **Determinism rule:** a layer generator must be a *pure function of world
> position + seed*. No `rand()`, no `time()`, no global mutable state — streamed
> chunks regenerate identically every time, or terrain will tear at chunk seams.
> See [`material-showcase`](../plugins/material-showcase/plugin.cpp) and
> [`ExamplePlugin`](../src/plugins/ExamplePlugin.cpp) for full heightmap and
> strata examples.

This already gets you a layered, multi-colored world: green surface over gray
stone, like `material-showcase`'s strata. But each individual block is one solid
color. For a block that is *grass on top and dirt on the sides*, read on.

---

## 4. Recipe: a Minecraft-style grass block

Here is the important part. The engine's native answer to "a grass block —
green top, brown sides, dirt below" is a **composite layer with a boundary-
override recipe**, not a per-face texture.

### Why this works

A `composite` voxel is a large "macro" block that stays atomic until something
needs its interior, at which point a **recipe** decomposes it into a grid of
smaller child voxels. A recipe can attach a **boundary override** to each face —
`top`, `bottom`, and `side` (the four lateral faces share one `side` spec) —
that paints the outer shell of the child grid with a different material than the
interior, to a configurable `depth`. So a macro block can decompose into:

- a **grass-green cap** on the top `depth` layers of child voxels,
- **dirt-brown** on the side and bottom shells,
- **dirt or stone** filling the interior.

Viewed from outside, that macro block reads exactly like a Minecraft grass
block: green on top, brown on the sides. The
[`recipe-world`](../plugins/recipe-world/plugin.cpp) plugin already ships a
version of this (a soil cap over a granite interior) — we'll specialize it into a
true grass block.

### The plugin

```cpp
#include "plugin_api.h"
#include "world/Voxel.h"
#include <cmath>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

namespace {

// Palette slots. 2 and 3 already default to green/brown in the base palette
// (src/renderer/Palette.h), but we set them explicitly so the block looks right
// regardless of any .vox import that may have overwritten those slots.
constexpr uint8_t kGrassIdx = 2;   // green top
constexpr uint8_t kDirtIdx  = 3;   // brown sides + bottom + interior

const double kBlockVoxelSizeM = 8.0;   // each macro "grass block" is 8 m
constexpr double kGroundTopM  = 24.0;  // solid ground from y=0 up to here

MaterialProperties make(uint8_t idx, float density, float hardness) {
    MaterialProperties m{};
    m.density             = density;
    m.structural_strength = 0.5f;
    m.hardness            = hardness;
    m.palette_index       = idx;
    return m;
}

// Coarse generator: a solid slab of macro blocks. Each macro voxel is "present"
// (solid) iff its vertical span overlaps the ground slab. Undecomposed, it
// renders as a single dirt-colored block; on approach the recipe below paints
// the grass cap. Pure function of position — no rand/time/global state.
void ground_generator(WorldCoord origin, int n, Voxel* out, void* user) {
    const double vs = user ? *static_cast<const double*>(user) : 1.0;
    const MaterialProperties block = make(kDirtIdx, 1500.0f, 0.4f);
    for (int z = 0; z < n; ++z)
      for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double bottom = origin.value.y + y * vs;
            const double top    = bottom + vs;
            Voxel& v = out[x + n * (y + n * z)];
            v = (top > 0.0 && bottom < kGroundTopM) ? Voxel{block} : Voxel::empty();
        }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // 1. Make the palette colors explicit.
    ctx->set_palette_color(ctx, kGrassIdx, 0xff228b22);  // forest green
    ctx->set_palette_color(ctx, kDirtIdx,  0xff2b5a8b);  // dirt brown (ABGR)

    // 2. Register the two materials the recipe will reference by id.
    ctx->register_material(ctx, "grass", make(kGrassIdx, 1200.0f, 0.2f));
    ctx->register_material(ctx, "dirt",  make(kDirtIdx,  1500.0f, 0.4f));

    // 3. Register the coarse layer generator. Pass the macro voxel size through
    //    user_data (the LayerGeneratorFn signature carries no voxel size).
    ctx->register_layer_generator(ctx, "ground", ground_generator,
                                  const_cast<double*>(&kBlockVoxelSizeM));

    // 4. The recipe: dirt interior, grass cap on top, dirt on the sides.
    MaterialWeight interiorMats[1] = {{"dirt", 1.0f}};   // bulk fill
    MaterialWeight grassCap[1]     = {{"grass", 1.0f}};  // top boundary
    MaterialWeight dirtSide[1]     = {{"dirt", 1.0f}};   // side boundary

    RecipeDesc recipe{};

    // Interior bulk distribution (a single material here; weights normalize
    // across the list when you have several). nullptr noise_id => built-in "value".
    recipe.interior.materials      = interiorMats;
    recipe.interior.material_count = 1;

    // TOP override: the grass cap. depth = how many child-voxel layers inward
    // from the top face it replaces. depth 1 = a one-voxel-thick green skin.
    recipe.top.present                     = true;
    recipe.top.depth                       = 1;
    recipe.top.distribution.materials      = grassCap;
    recipe.top.distribution.material_count = 1;

    // SIDE override: dirt on all four lateral faces. (Bottom is left to the
    // interior, which is already dirt.) Overlap order at edges is fixed:
    // bottom -> side -> top, so the grass top always wins the rim. See
    // architecture.md §6 "Boundary Overrides".
    recipe.side.present                     = true;
    recipe.side.depth                       = 1;
    recipe.side.distribution.materials      = dirtSide;
    recipe.side.distribution.material_count = 1;

    ctx->register_recipe(ctx, "ground", &recipe);   // deep-copied; arrays needn't outlive the call
    return 0;
}
```

### Wiring up the layers

A composite layer needs a finer child layer to decompose *into*. Declare the
layer stack in your demo/game's project config (see the YAML examples in the
[README](../README.md) "Multi-Layer Scale System" and the
`05-decompose-on-approach` / `09-recipe-built-voxel` demos):

```yaml
layers:
  - name: ground          # the composite grass blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
  - name: terrain         # the 1 m child voxels the recipe paints
    voxel_size_m: 1.0
    mode: terminal
```

When the player approaches (or clicks) a `ground` block, it decomposes into an
8×8×8 grid of 1 m voxels: the top layer grass-green, the sides and interior
dirt-brown. That is your grass block.

### Going richer

The same recipe machinery scales up to genuinely interesting blocks:

- **Multi-material interiors** — give `interior.materials` several
  `MaterialWeight`s and a `noise_id` (`"value"`, `"fbm"`, `"ridged"`,
  `"worley"`) so the bulk is, say, 80% stone / 20% iron-ore arranged by noise.
- **Thicker caps** — bump `top.depth` to 2–3 for a multi-voxel topsoil layer.
- **Feature overlays** — register `FeatureGeneratorFn`s (caves, ore veins) and
  list them in `recipe.features`; they stamp structures into the child grid
  *after* the material distribution. `recipe-world` carves caves and threads ore
  this way.
- **Snow caps, exposed rock, ice shells** — any per-face look is just a
  different boundary distribution.

---

## 5. Going further: true per-face textures

If you specifically want **image textures** (a grass-blade texture sampled
across a face, not a flat color), the engine does not support that today, and it
is a deliberate, documented gap rather than an accident. A single terminal voxel
is one solid shaded color. To add real per-face textures you would need to
extend the engine itself (not just a plugin):

1. **Vertex format** — add UV (and likely a face/material id) to `MeshVertex`
   in [`ChunkMeshData.h`](../src/renderer/ChunkMeshData.h) and emit them in the
   face loop of `ChunkMeshData.cpp`.
2. **A texture atlas** — load an atlas image and register it with bgfx (there is
   currently no image asset pipeline; `assets/` holds only audio).
3. **Shaders** — extend `shaders/vs_voxel.sc` / `fs_voxel.sc` to pass UVs through
   and sample the atlas, then rebuild the committed bytecode in
   `shaders/generated/`.
4. **Per-face material selection** — decide where face textures come from. The
   palette is per-voxel, so you'd add a mapping from `palette_index` (or a new
   material field) to atlas tiles per face direction (`kFaces` already labels the
   six directions).

The architecture already anticipates this — `MaterialProperties::palette_index`
is documented as mapping to "color, texture, PBR params" — so the data model has
room. It's the vertex format and shader that need the work. If you go this
route, read `docs/architecture.md` §9 (shaders/chunk format) first, and treat it
as an engine change with its own design discussion.

For most "interesting voxel" goals, though — colored materials, translucency,
and grass-block-style multi-face blocks — the material + recipe path in
[§3](#3-recipe-a-new-colored-material) and
[§4](#4-recipe-a-minecraft-style-grass-block) is the intended, fully supported
way, and requires no engine changes at all.

---

## 6. Build and run

Drop your `plugin.cpp` into `plugins/<your-name>/`, then:

```bash
cmake -B build
cmake --build build
# Your plugin builds to build/plugins/<your-name>.so (.dylib / .dll elsewhere).
```

The plugin is discovered automatically — no CMake edits. Load it from a demo via
`PluginManager`, the way `03-plugin-driven-world` loads plugins from
`build/plugins/`. To see a composite recipe decompose, follow the
`09-recipe-built-voxel` demo (fly toward a block, or left-click it).

---

## 7. Reference

| What | Where |
|------|-------|
| Material properties struct | [`include/plugin_api.h`](../include/plugin_api.h) `MaterialProperties` |
| Voxel definition | [`src/world/Voxel.h`](../src/world/Voxel.h) |
| Palette & default colors | [`src/renderer/Palette.h`](../src/renderer/Palette.h) |
| Mesh builder & per-face shading | [`src/renderer/ChunkMeshData.cpp`](../src/renderer/ChunkMeshData.cpp) |
| Registration entry points (`register_material`, `set_palette_color`, `register_recipe`, …) | [`include/plugin_api.h`](../include/plugin_api.h) `PluginContext` |
| Recipe & boundary structs | [`include/plugin_api.h`](../include/plugin_api.h) `RecipeDesc`, `BoundaryDesc`, `MaterialWeight` |
| Simplest material + generator example | [`src/plugins/ExamplePlugin.cpp`](../src/plugins/ExamplePlugin.cpp) |
| Multi-material strata example | [`plugins/material-showcase/plugin.cpp`](../plugins/material-showcase/plugin.cpp) |
| Full boundary-recipe example (grass-block analog) | [`plugins/recipe-world/plugin.cpp`](../plugins/recipe-world/plugin.cpp) |
| Design rationale | [`docs/architecture.md`](architecture.md) §5, §6 |
</content>
</invoke>

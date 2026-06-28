# Tutorial 05: Multi-Face Blocks via Recipes

Give your voxels visual variety -- green grass on top, brown dirt on the sides
-- using boundary overrides in the engine's recipe system. No engine changes
required; everything lives in a plugin.

---

## Prerequisites

- Engine built and running ([Tutorial 01](01-hello-voxel.md))
- Comfortable writing a plugin ([Tutorial 02](02-your-first-plugin.md))
- Familiar with materials and palette colors ([Tutorial 03](03-materials-and-properties.md))
- Understanding of composite vs. terminal layers ([Tutorial 04](04-composition-recipes.md))

---

## 1. The problem: one voxel, one color

A single terminal voxel renders as one solid color with per-face brightness
shading (top brightest, bottom darkest). The mesh builder looks up
`palette::color(palette_index)` and emits that color for every visible face.
There is no texture atlas, no UV coordinates -- the vertex format is
`{float x, y, z; uint32_t abgr}`.

So how do you build a Minecraft-style grass block where the top is green and
the sides are brown?

---

## 2. The engine-native answer: composite blocks with boundary overrides

A `composite` voxel is a large macro block that stays atomic until something
needs its interior. When it decomposes, a **recipe** fills the resulting child
grid with materials. The recipe can attach **boundary overrides** to three face
groups -- `top` (+Y), `bottom` (-Y), and `side` (all four lateral faces) --
that paint the outer shell of the child grid with a different material than the
interior, to a configurable depth.

The result: a macro block that decomposes into a green cap on top, brown on the
sides, and brown (or stone) filling the interior. Viewed from outside, it looks
exactly like a multi-face block.

---

## 3. BoundaryDesc: the face override struct

Each face group is described by a `BoundaryDesc` (defined in
[`include/plugin_api.h`](../../include/plugin_api.h)):

```cpp
struct BoundaryDesc {
    DistributionDesc distribution;  // material distribution for this face
    int              depth   = 1;   // child-voxel layers inward from face
    bool             present = false; // false = face uses interior distribution
};
```

| Field | Purpose |
|-------|---------|
| `present` | When `false` (default), this face group uses the interior distribution -- no override. Set to `true` to activate the override. |
| `depth` | How many child-voxel layers inward from the face are replaced by this override's distribution. `1` = a one-voxel-thick skin; `2` = a two-voxel topsoil. |
| `distribution` | A `DistributionDesc` specifying which materials fill this face band, with optional noise. |

The distribution itself:

```cpp
struct DistributionDesc {
    const MaterialWeight* materials         = nullptr;
    size_t                material_count    = 0;
    const char*           noise_id          = nullptr;  // nullptr => built-in "value"
    const RecipeParam*    noise_params      = nullptr;
    size_t                noise_param_count = 0;
};
```

And a `RecipeDesc` carries three boundary fields:

```cpp
struct RecipeDesc {
    DistributionDesc interior;       // the bulk distribution
    // ...
    BoundaryDesc     top;            // +Y face
    BoundaryDesc     bottom;         // -Y face
    BoundaryDesc     side;           // all four lateral faces
    // ...
};
```

---

## 4. Overlap order: bottom, side, top

At edges and corners where two or more boundary zones overlap, the engine
applies them in a fixed order: **bottom, then side, then top**. Top wins at the
rim. This means the grass cap covers the top corners even where side boundaries
also claim those voxels.

```
         top wins here
            v v v
        +---+---+---+
        | T | T | T |   <- top boundary (depth 1)
        +---+---+---+
        | S |   | S |   <- side boundary (depth 1)
        +---+---+---+
        | S |   | S |
        +---+---+---+
        | B | B | B |   <- bottom boundary (depth 1)
        +---+---+---+

  Cross-section of an 4x4 child grid.
  T = top override, S = side override, B = bottom override.
  Interior cells use the recipe's interior distribution.
```

---

## 5. Step-by-step: building a grass block

This walkthrough builds a composite grass block: green top, dirt sides and
interior.

### 5.1 Register the materials

```cpp
MaterialProperties dirt{};
dirt.density = 1400.0f;
dirt.hardness = 0.3f;
dirt.palette_index = 5;
ctx->register_material(ctx, "dirt", dirt);
ctx->set_palette_color(ctx, 5, 0xff2d5e87);  // ABGR brown

MaterialProperties grass_surface{};
grass_surface.density = 1200.0f;
grass_surface.hardness = 0.2f;
grass_surface.palette_index = 6;
ctx->register_material(ctx, "grass_surface", grass_surface);
ctx->set_palette_color(ctx, 6, 0xff30a040);  // ABGR green
```

Palette colors are **ABGR** packed `uint32_t`: `0xAABBGGRR`. Alpha `0xff` is
opaque; alpha below `0xff` makes the material translucent.

### 5.2 Define the interior distribution

The interior is the bulk of the block -- everything not covered by a boundary
override. Here, 100% dirt:

```cpp
MaterialWeight interiorMats[] = {{"dirt", 1.0f}};
DistributionDesc interior{};
interior.materials = interiorMats;
interior.material_count = 1;
```

### 5.3 Define the top boundary

The green surface cap, one child-voxel layer deep:

```cpp
BoundaryDesc top{};
top.present = true;
top.depth = 1;
MaterialWeight topMats[] = {{"grass_surface", 1.0f}};
top.distribution.materials = topMats;
top.distribution.material_count = 1;
```

### 5.4 Assemble and register the recipe

```cpp
RecipeDesc recipe{};
recipe.interior = interior;
recipe.top = top;
// bottom and side left as default (present = false -> uses interior)
ctx->register_recipe(ctx, "blocks", &recipe);
```

Because `bottom` and `side` are left with `present = false`, those faces use
the interior distribution (dirt). Only the top gets the green override.

The engine deep-copies the recipe at registration, so the local arrays need not
outlive the `register_recipe` call.

### 5.5 Wire up the layer config

The composite layer must declare `mode: composite` and `decompose_to:` a child
layer in the YAML config:

```yaml
layers:
  - name: blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 8
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    interactive: true
```

When the player approaches a `blocks` macro voxel, it decomposes into an 8x8x8
grid of 1 m child voxels: the top layer green, everything else dirt. For full
details on layer configs, see
[`docs/configuration-guide.md`](../configuration-guide.md).

---

## 6. Going richer

### Multi-material interiors with noise

Instead of a single dirt fill, mix materials using weighted noise:

```cpp
MaterialWeight interiorMats[] = {{"granite", 0.7f}, {"basalt", 0.3f}};
interior.materials = interiorMats;
interior.material_count = 2;
interior.noise_id = "fbm";
```

The `noise_id` selects a spatial noise function. Built-in options: `"value"`,
`"fbm"`, `"ridged"`, `"worley"`. Register custom noise via
`ctx->register_noise()`, or consume a built-in from a plugin via
`ctx->resolve_noise()`.

### Thicker caps

For a multi-voxel topsoil layer, increase `depth`:

```cpp
recipe.top.depth = 3;  // three child-voxel layers of grass
```

### Snow-capped mountain block

Set `top.depth = 2` with a snow material for a thick snow cap:

```cpp
MaterialWeight snowMats[] = {{"snow", 1.0f}};
recipe.top.distribution.materials = snowMats;
recipe.top.distribution.material_count = 1;
recipe.top.depth = 2;
```

### Exposed-rock side faces

Activate the side boundary with a rock material:

```cpp
recipe.side.present = true;
recipe.side.depth = 1;
MaterialWeight rockMats[] = {{"exposed_rock", 1.0f}};
recipe.side.distribution.materials = rockMats;
recipe.side.distribution.material_count = 1;
```

### Ice shell block

Combine all three boundaries for an ice-shell block with a different interior:

```cpp
recipe.top.present = true;
recipe.bottom.present = true;
recipe.side.present = true;
// All three use ice; interior uses packed snow or air
```

Remember the overlap order: at the top rim, the top boundary wins over the side
boundary. At the bottom rim, the side boundary wins over the bottom boundary.

For the complete recipe-book reference, including feature overlays and seed
parameters, see [`docs/creating-voxels.md`](../creating-voxels.md) section 4.

---

## How to verify

1. Build and run the recipe demo:

   ```bash
   cmake -B build && cmake --build build
   ./build/09-recipe-built-voxel
   ```

   Fly toward a composite block. It should decompose into a child grid with
   visibly different colors on the top face versus the sides.

2. To see the boundary override in a custom plugin, add the grass block recipe
   from section 5 to your plugin, configure the two-layer YAML from section
   5.5, and observe the green cap on decomposition.

---

## Key references

| What | Where |
|------|-------|
| BoundaryDesc, RecipeDesc, DistributionDesc | [`include/plugin_api.h`](../../include/plugin_api.h) |
| MaterialWeight struct | [`include/plugin_api.h`](../../include/plugin_api.h) |
| Recipe-world plugin (ships a boundary-override recipe) | `plugins/recipe-world/plugin.cpp` |
| Full recipe-book reference (grass block and beyond) | [`docs/creating-voxels.md`](../creating-voxels.md) section 4 |
| Layer config fields and validation | [`include/core/LayerConfig.h`](../../include/core/LayerConfig.h) |
| Configuration guide | [`docs/configuration-guide.md`](../configuration-guide.md) |
| Architecture: composition recipes | [`docs/architecture.md`](../architecture.md) section 6 |

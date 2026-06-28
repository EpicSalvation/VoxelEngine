# Tutorial 04: Composition Recipes

Build a procedural multi-scale world where macro voxels decompose into richly
detailed interiors using recipes -- material distributions, noise fields,
boundary overrides, and feature overlays.

---

## Prerequisites

- The engine builds and runs successfully (see
  [Tutorial 01](01-hello-voxel.md))
- You can write and register a plugin (see
  [Tutorial 02](02-your-first-plugin.md))
- You understand `MaterialProperties` and palette colors (see
  [Tutorial 03](03-materials-and-properties.md))

---

## 1. Voxel modes: composite, terminal, immutable

Every layer in a `LayerConfig` has a **mode** that controls its behavior:

| Mode | Behavior |
|------|----------|
| `terminal` | The editable leaf layer. Players build and mine here. No further decomposition. |
| `composite` | A macro voxel that **lazily decomposes** into a finer child layer on approach or interaction. Must name a `decompose_to` target. |
| `immutable` | Render and collision only. Never modified, never decomposed, never persisted. Used for distant backdrops. |

A typical multi-scale world stacks composites on top of a terminal leaf:

```yaml
layers:
  - name: blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 4
    view_distance_chunks: 7
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 32
    view_distance_chunks: 5
```

When the player approaches an 8 m "blocks" voxel, the engine decomposes it
into an 8x8x8 grid of 1 m "terrain" voxels. The **recipe** registered for
the "blocks" layer controls what that interior looks like.

---

## 2. The decomposition chain

Decomposition is **cascading** and **lazy**: a macro voxel decomposes one
layer at a time, and only when something needs its interior (camera proximity
or player interaction). In a three-layer stack
(`continental -> regional -> terrain`), an approaching player first triggers
the continental-to-regional decomposition, then regional-to-terrain.

Each step is driven by a **recipe** -- a declarative description of what the
interior contains. The engine never jumps scales in a single step.

When the player moves away, the fine-grained child chunks are collapsed back
to coarse macro voxels, keeping the resident mesh count bounded.

---

## 3. Writing a RecipeDesc

A `RecipeDesc` is a flat POD struct that describes how a composite macro voxel
fills its child grid when it decomposes. It has four parts:

1. **Interior distribution** -- the bulk material fill
2. **Boundary overrides** -- face-specific material shells (top, bottom, side)
3. **Feature overlays** -- structures stamped into the grid after filling
4. **Seed parameters** -- values cascaded to the child layer's generation

### Interior: DistributionDesc

The interior distribution defines the bulk material fill. It supports multiple
weighted materials arranged spatially by a named noise field:

```cpp
MaterialWeight mats[] = {
    {"granite", 0.8f},
    {"basalt",  0.15f},
    {"quartz",  0.05f}
};

DistributionDesc interior{};
interior.materials      = mats;
interior.material_count = 3;
interior.noise_id       = "fbm";  // spatial arrangement

RecipeParam noiseParams[] = {
    {"scale", RecipeParamKind::Number, 4.0, nullptr}
};
interior.noise_params      = noiseParams;
interior.noise_param_count = 1;
```

Built-in noise IDs: `"value"`, `"fbm"`, `"ridged"`, `"worley"`. Pass
`nullptr` for `noise_id` to use the default (`"value"`). The noise field
determines which material is placed at each position -- the weights are
thresholds on the noise value, so heavier-weighted materials occupy more of
the volume.

### Boundary overrides: BoundaryDesc

Boundary overrides paint the outer shell of the child grid with a different
material than the interior. Each face direction (`top`, `bottom`, `side`) can
have its own override:

```cpp
BoundaryDesc top{};
top.present = true;
top.depth   = 1;  // 1 child-voxel layer from the top face

MaterialWeight soilMats[] = {{"soil", 1.0f}};
top.distribution.materials      = soilMats;
top.distribution.material_count = 1;
```

`depth` controls how many child-voxel layers inward from the face are
replaced. A depth of 1 creates a one-voxel-thick skin; depth 2-3 creates a
thicker cap.

**Overlap order at edges and corners:** `bottom -> side -> top`. When two
boundaries compete for the same voxel at an edge or corner, the top override
wins. This means a grass-cap always covers the rim of a block, even at the
edges.

For a walkthrough of building a Minecraft-style grass block using boundary
overrides, see [`docs/creating-voxels.md`](../creating-voxels.md) section 4.

### Feature overlays: FeatureRef

Features are structures stamped **in-place** into the child grid after the
interior and boundaries are filled. They operate on the already-populated
voxel array, carving or adding material:

```cpp
RecipeParam caveParams[] = {
    {"threshold", RecipeParamKind::Number, 0.45, nullptr},
    {"scale",     RecipeParamKind::Number, 8.0,  nullptr}
};

RecipeParam oreParams[] = {
    {"density", RecipeParamKind::Number, 0.02, nullptr}
};

FeatureRef features[] = {
    {"cave",     caveParams, 2},
    {"ore_vein", oreParams,  1}
};
```

Features are applied in array order. In this example, caves carve first, then
ore veins are threaded through the remaining solid material.

### Seed parameters: RecipeParam

Seed parameters bias the child layer's generation. They are cascaded
downward through the decomposition chain:

```cpp
RecipeParam seedParams[] = {
    {"cave_density", RecipeParamKind::Number, 0.3, nullptr}
};
```

The engine also injects `"__altitude"` automatically into every recipe's
effective parameters -- this is the decomposing macro voxel's world-space
height, so features can vary with depth without the plugin computing it.

---

## 4. Assembling the complete recipe

```cpp
RecipeDesc recipe{};
recipe.interior = interior;               // bulk distribution
recipe.features      = features;          // feature overlays
recipe.feature_count = 2;
recipe.top    = topBoundary;              // face overrides
recipe.bottom = bottomBoundary;
recipe.side   = sideBoundary;
recipe.seed_parameters      = seedParams;
recipe.seed_parameter_count = 1;

ctx->register_recipe(ctx, "blocks", &recipe);  // deep-copied by engine
```

The engine deep-copies the `RecipeDesc` at registration. The plugin's arrays
do not need to outlive the `register_recipe` call.

---

## 5. Writing a feature generator

A `FeatureGeneratorFn` operates **in-place** on an already-filled voxel grid.
It receives the effective parameters (the recipe entry's own params merged
with inherited seed parameters) and a deterministic per-decomposition seed:

```cpp
void cave_feature(WorldCoord origin, double voxel_size, int grid_size,
                  Voxel* inout,
                  const RecipeParam* params, size_t count,
                  uint64_t seed, void* user_data) {
    double threshold = recipe_param_num(params, count, "threshold", 0.45);

    for (int z = 0; z < grid_size; ++z)
        for (int y = 0; y < grid_size; ++y)
            for (int x = 0; x < grid_size; ++x) {
                WorldCoord pos(origin.value.x + x * voxel_size,
                               origin.value.y + y * voxel_size,
                               origin.value.z + z * voxel_size);
                uint64_t s = voxel_seed_mix(seed,
                                 x + grid_size * (y + grid_size * z));
                float n = voxel_rng_norm(&s);
                if (n < threshold)
                    inout[x + grid_size * (y + grid_size * z)] = Voxel::empty();
            }
}

ctx->register_feature_generator(ctx, "cave", cave_feature, nullptr);
```

`recipe_param_num` is a header-only helper in `plugin_api.h` that looks up a
named parameter by key and returns a fallback if not found.

The same determinism rule as layer generators applies: the same seed must
produce the same result. Use `voxel_rng_next`, `voxel_rng_norm`, and
`voxel_seed_mix` for all randomness.

---

## 6. Noise functions

### Consuming built-in noise

A plugin can resolve a built-in (or plugin-registered) noise function by ID:

```cpp
NoiseFn valueNoise = ctx->resolve_noise(ctx, "value");
float n = valueNoise(pos, seed, params, param_count, nullptr);
```

`resolve_noise` returns `nullptr` if the ID is not registered, so check
before calling.

### Registering custom noise

```cpp
float my_noise(WorldCoord pos, uint64_t seed,
               const RecipeParam* params, size_t count, void* user_data) {
    // Return a scalar in [0,1)
    uint64_t s = voxel_seed_mix(seed,
        static_cast<uint64_t>(pos.value.x * 1000.0));
    return voxel_rng_norm(&s);
}

ctx->register_noise(ctx, "my_noise", my_noise, nullptr);
```

A registered noise ID overrides a built-in of the same name. Noise functions
are sampled at **world positions** (not chunk-local positions), so adjacent
macro voxels' child grids are seamless.

---

## 7. Deterministic RNG helpers

All three helpers are defined inline in `plugin_api.h` and compile with no
engine dependency:

```cpp
uint64_t voxel_rng_next(uint64_t* state);       // splitmix64
float    voxel_rng_norm(uint64_t* state);        // uniform [0,1)
uint64_t voxel_seed_mix(uint64_t a, uint64_t b); // fold two ints into a seed
```

Use `voxel_seed_mix` to derive a per-voxel seed from the chunk seed and the
voxel's flat index:

```cpp
uint64_t s = voxel_seed_mix(seed, x + grid_size * (y + grid_size * z));
float roll = voxel_rng_norm(&s);
```

This guarantees that the same world position produces the same random
sequence on every run, on every platform, regardless of thread scheduling.

---

## 8. Driving the decomposition

In a demo or game, the `DecompositionManager` drives the cascade. It
decomposes composite voxels within an approach radius of the camera and
collapses distant ones to keep the resident chunk count bounded:

```cpp
DecompositionManager decompMgr(world, pluginManager, layerConfig, kWorldSeed);

// In the game loop, each frame:
auto diffs = decompMgr.tick(camPos, approachRadius,
                            loadPerFrame, decompPerFrame, applyPerFrame);
```

The budgets (`loadPerFrame`, `decompPerFrame`, `applyPerFrame`) control how
much work the manager does per frame to avoid stalls. See
`demos/09-recipe-built-voxel/main.cpp` for the full integration pattern.

---

## How to verify

1. **Build and run the recipe demo:**

   ```bash
   cmake -B build && cmake --build build
   ./build/09-recipe-built-voxel
   ```

2. **Observe recipe-driven decomposition:**
   - Fly toward a macro voxel. As you approach, it decomposes into a detailed
     interior: soil cap on top, granite/basalt bulk, cave voids, and ore veins.
   - Dig into a decomposed block to see the cross-section: the boundary
     overrides (soil on top), the noise-driven material distribution (granite
     and basalt), and the carved cave features.

3. **Verify determinism:** Fly away so the fine terrain collapses back to
   coarse blocks. Return to the same area -- every block regenerates
   identically (byte-for-byte). The decomposition is a pure function of
   position and seed.

4. **Depth-dependent caves:** Dig a shaft straight down. Caves near the
   surface are sparse; deeper caves become denser. This is the
   engine-cascaded `"__altitude"` parameter at work -- the cave feature
   reads the macro voxel's height from the effective params and adjusts its
   threshold.

---

## Key references

| What | Where |
|------|-------|
| RecipeDesc, DistributionDesc, BoundaryDesc, FeatureRef | `include/plugin_api.h` |
| NoiseFn and RNG helpers | `include/plugin_api.h` |
| Recipe-world plugin (full recipe example) | `plugins/recipe-world/plugin.cpp` |
| Recipe-built-voxel demo | `demos/09-recipe-built-voxel/main.cpp` |
| DecompositionManager | `src/world/DecompositionManager.h` |
| Composition recipe design | [`docs/architecture.md`](../architecture.md) section 6 |
| Creating voxels recipe book | [`docs/creating-voxels.md`](../creating-voxels.md) |
| Layer config (composite mode) | [`docs/configuration-guide.md`](../configuration-guide.md) |
| Material registration | [Tutorial 02](02-your-first-plugin.md) |
| Material properties | [Tutorial 03](03-materials-and-properties.md) |

# Proposal: Recipe Occupancy (carve-to-surface for recipe-driven decomposition)

Status: **implemented — M18.5** (Composite Heightmap Worlds breakfix; see README milestones).
Origin: M10 fix-plan item 15 (`docs/m10-incremental-decomposition-plan.md`); re-surfaced
by the M18 Mega-Demo (`docs/m18-mega-demo-metrics.md`, "A note on scope honesty"), which hit
the same limitation from the M13-collapse angle — a heightmap world has no composite macros.

> **As built (M18.5).** Shipped as designed below, with three notes: (1) `OccupancyDesc`
> is **appended** to `RecipeDesc` (not placed first) per the ABI append-only rule, and the
> plugin ABI version was bumped 2 → 3. (2) The v2 surface boundary landed in the same
> milestone as a `BoundaryMode::Surface` flag on `BoundaryDesc` — depth measured from the
> carved surface per column (worker-only second pass in `fillChildChunk`). (3) The
> "coarse-supersets-fine" half is now **engine-derived**: a root composite layer with an
> occupancy recipe and no registered generator has its coarse occupancy synthesized by
> `DecompositionManager` from the same carve field (sampled over each macro's footprint with
> the same per-macro occupancy seed), so the surface lives in one place and the superset
> invariant is guaranteed rather than authored. The occupancy salt is shared
> (`kRecipeOccupancySalt`) so coarse and fine occupancy sample an identical field.

## Problem

A recipe describes only the *content* of a solid macro voxel, never its
*shape*: `fillChildChunk` (`src/world/ResolvedRecipe.cpp`) assigns a material
to every child cell inside the macro's subvolume — `sampleDistribution` cannot
return empty unless the distribution itself is empty, and then the whole macro
is empty. A recipe therefore refines a solid cube into an identical solid cube
of smaller voxels.

That is correct for fully interior macros, but it makes recipes unusable for
any macro the surface passes through: no surface contour, no caves, no hollow
core. The M10 demo hit this directly — its recipe-driven cascade produced
full-cube "refinement" and the cave-carved terrain generator was dead code —
and was switched to the M6 generator-fallback path as a workaround (plan items
3/4). Until recipes can carve, every surface-crossing layer must be
generator-driven, which forfeits the recipe system's authoring model and the
M10 seed-parameter cascade for exactly the macros players see most.

## Goals and constraints

1. **Carve only.** Occupancy may turn a child cell empty; it must never create
   solid cells outside the macro (coarse-supersets-fine, ARCHITECTURE §4, holds
   by construction — same as today's bounds check in `fillChildChunk`).
2. **Deterministic.** Pure function of (recipe, macro coord, decomposition
   seed, params). Same salting discipline as the distribution/feature seeds.
3. **§13 boundary.** All id→pointer resolution happens at job-build time on the
   main thread (`resolveRecipe`); `DecompositionWorker` never touches
   `PluginManager`.
4. **C ABI friendly.** Additive change to `RecipeDesc` (`include/plugin_api.h`);
   zero-initialized = absent = today's behavior, so existing plugins are
   unaffected.
5. **Seed-cascade aware.** Occupancy params participate in the M10 inherited
   `seed_parameters` merge, including the reserved `__altitude` /
   `__parent_material` keys, so a parent can bias its children's carving
   (e.g. `cave_density`).

## Design

### ABI (plugin_api.h)

```c
// Optional occupancy stage: decides which child cells of a decomposing macro
// are solid at all, before any material is assigned. Zero-initialized
// (present == false) means "fully solid" — exactly today's behavior.
struct OccupancyDesc {
    const char*        noise_id    = nullptr;  // nullptr => built-in "value"
    float              threshold   = 0.0f;     // solid iff noise >= threshold
    const RecipeParam* params      = nullptr;  // merged over inherited seed params
    size_t             param_count = 0;
    bool               present     = false;
};

struct RecipeDesc {
    OccupancyDesc      occupancy;   // NEW — appended; existing initializers keep working
    DistributionDesc   interior;
    ...
};
```

A `NoiseFn` (already in the ABI: `float(WorldCoord, seed, params, count, user)`)
is expressive enough for real surfaces: a plugin registers a noise whose value
encodes signed distance to its surface field (e.g. the drill world's
`surfaceHeight`/crust band as `register_noise("drill_crust", ...)`), and the
recipe sets `noise_id = "drill_crust", threshold = 0.5`. No new function-pointer
type is needed; that keeps validation (`validateRecipes`) and resolution
(`resolveRecipe`) on the existing noise path.

### Resolution (main thread, §13)

`ResolvedRecipe` gains a `ResolvedOccupancy { present, threshold, NoiseFn,
user, params }`. `resolveRecipe` resolves `noise_id` exactly like a
distribution noise and merges params with the standard precedence
(inherited ancestor seed params → recipe `seed_parameters` → occupancy's own
`params`). `validateRecipes` checks the id resolves, as it does for
distribution noise ids.

### Evaluation (worker)

In `fillChildChunk`, per child cell, **before** the distribution sample:

```
if (recipe.occupancy.present &&
    occupancyNoise(cellCenter, occSeed, params) < threshold) {
    chunk.at(x,y,z) = Voxel::empty();
    continue;
}
```

- `occSeed = voxel_seed_mix(decompSeed, kOccupancySalt)` with a new fixed salt
  constant, decorrelating it from the distribution (`kDistributionSalt`) and
  feature (`index+1`) domains.
- Feature overlays still run after the distribution pass and may write empty or
  solid voxels as they do today (features are explicit author intent; the
  occupancy stage only gates the *default* fill).

### Boundary interaction (the soil-cap question)

Today's `top/bottom/side` boundaries are macro-aligned slabs. With carving, a
"top" cap paints the top of the *macro*, not the top of the *carved surface* —
a soil cap would land in the air above a carved hillside. Two stages:

- **v1 (this proposal):** keep boundaries macro-aligned and document the
  limitation. Occupancy + interior alone already covers the drill-world use
  case (rock crust with caves); caps can be a feature overlay that reads the
  same surface noise.
- **v2 (follow-up):** add a `surface` boundary mode — depth measured downward
  from the highest solid cell in each column of the carved result. Worker-only
  change (a per-column second pass in `fillChildChunk`), no ABI impact beyond a
  mode flag on `BoundaryDesc`.

### Coarse-supersets-fine

Unchanged and automatic: occupancy only empties cells inside a solid parent.
The *generator-side* authoring contract from ARCHITECTURE §4 still applies in
reverse — a coarse layer must mark a macro present wherever its descendants'
occupancy could be solid. When both derive from one registered noise field
(the intended pattern), the coarse generator samples the same field
conservatively, which is exactly how the drill-world generators already work.

### Migration path for demo 10

Re-register recipes for all three composite hops with
`occupancy = { "drill_crust", 0.5 }` (a plugin noise wrapping
`cellSolid`/`surfaceHeight`), reinstating the recipe path — and the M10
seed-parameter cascade showcase — without regressing the carved silhouette
that the generator fallback currently provides. The generator path remains for
plugins that prefer it; `makeJob`'s recipe-preferred selection is unchanged.

## Out of scope

- Exposure-aware boundary painting (already deferred in ARCHITECTURE §6).
- Multi-field occupancy composition (CSG of several noises) — author a single
  registered noise instead.
- Per-material occupancy (different thresholds per material) — use features.

## Implementation sketch (when scheduled)

1. `plugin_api.h`: `OccupancyDesc`, field on `RecipeDesc` (additive).
2. `Recipe.h` / `PluginManager` deep-copy: `OccupancyValue`.
3. `RecipeResolve`: `ResolvedOccupancy`, resolve + validate.
4. `ResolvedRecipe.cpp`: occupancy gate in `fillChildChunk`, `kOccupancySalt`.
5. Tests: carve determinism (same seed ⇒ same holes), superset invariant
   (no solid child outside a solid parent), absent-occupancy byte-compatibility
   with today's output, threshold edge cases (0 ⇒ all solid for noise ≥ 0,
   >1 ⇒ all empty).
6. Demo 10: `drill_crust` noise + recipes restored (validates the migration
   path end to end).

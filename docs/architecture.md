# Architecture Guide

This document explains the internal design of the Voxel Game Engine at the subsystem level. It covers *why* decisions were made, not just *what* they are. It is intended for contributors extending the engine and for AI coding agents working within the codebase.

If you are an AI agent: read this document before modifying any subsystem. The constraints listed here are not stylistic preferences — they are load-bearing. Violating them silently produces incorrect behavior that is difficult to diagnose.

---

## Table of Contents

1. [Coordinate System](#1-coordinate-system)
2. [Layer Configuration and Validation](#2-layer-configuration-and-validation)
3. [Voxel Modes](#3-voxel-modes)
4. [Cascading Decomposition](#4-cascading-decomposition)
5. [Material Property System](#5-material-property-system)
6. [Composition Recipes and Feature Generators](#6-composition-recipes-and-feature-generators)
7. [Upward Damage Propagation](#7-upward-damage-propagation) (**experimental**)
8. [Plugin System](#8-plugin-system)
9. [Renderer and LOD](#9-renderer-and-lod)
10. [Import/Export and Editor Interoperability](#10-importexport-and-editor-interoperability)
11. [Persistence and Dirty Tracking](#11-persistence-and-dirty-tracking)
12. [Build, Packaging, and the Engine Library Boundary](#12-build-packaging-and-the-engine-library-boundary)
13. [Subsystem Dependency Map](#13-subsystem-dependency-map)
14. [Guidance for AI Coding Agents](#14-guidance-for-ai-coding-agents)
15. [Networking and Multiplayer](#15-networking-and-multiplayer)
16. [Audio](#16-audio)
17. [Fluid and Thermal Simulation](#17-fluid-and-thermal-simulation)
18. [Gravity Provider and Axis-Agnostic Kinematics](#18-gravity-provider-and-axis-agnostic-kinematics)

---

## 1. Coordinate System

**Files:** `include/WorldCoord.h`

### The Problem

A 32-bit float has approximately 7 significant decimal digits of precision. At 100km world scale (100,000m), this means the smallest representable difference near the origin is roughly 0.01m — 1cm. At 100km *from* the origin, floating-point gaps widen further. Sub-meter building detail at planetary scale is simply not possible with float coordinates.

### The Solution: `WorldCoord` + Floating Origin

All world-space positions are represented as `WorldCoord`, a type that wraps a `glm::dvec3` (64-bit double precision). This gives ~15 significant digits — enough to represent millimeter detail across a solar-system-scale world.

The GPU only receives 32-bit floats. Before submission, all geometry is translated into **camera-local space**: the camera's `WorldCoord` is subtracted from every vertex position, yielding a local float vector that is always small in magnitude and therefore precise. This technique is standard in flight simulators and large-scale open world engines.

### Rules

- `WorldCoord` is the only permitted type for world-space positions in engine code and plugins
- Raw `double`, `float`, or `glm::vec3` must not appear in world-space position calculations
- `WorldCoord` provides explicit conversion methods: `toLocalFloat(origin)` for GPU submission only
- Implicit conversion to float is deliberately not provided — accidental promotion is a compile error

### Why This Cannot Be Retrofitted

If raw floats are used early in development, precision errors compound silently into mesh seams, physics jitter, and collision failures that only appear at large distances from the origin. Fixing this after the fact requires auditing every position in the codebase. Defining the type early costs almost nothing.

---

## 2. Layer Configuration and Validation

**Files:** `include/core/LayerConfig.h`, `src/core/LayerConfig.cpp`

### Purpose

`LayerConfig` parses the game's layer stack from the project config file and validates it before any world systems initialize. All downstream systems trust that a valid `LayerConfig` means the layer stack is coherent — they do not re-validate.

### Validation Performed at Startup

| Check | Error behavior |
|---|---|
| At least one layer defined | Hard error, engine exits |
| Each layer size < parent size | Hard error |
| Parent/child size ratio is a whole integer | Hard error with ratio printed |
| Ratio >= 2:1 between adjacent layers | Hard error |
| Every `composite` layer has a `decompose_to` target that exists | Hard error |
| Every `composite` layer has a recipe plugin registered | Hard error |
| At most one layer flagged `interactive: true` (M16, L4) | Hard error, layer names printed |
| Single-decomposition child grid count exceeds `max_decomposition_voxels` config | Warning, child count printed |

### Why Hard Errors, Not Warnings

A composite layer with no recipe, or a non-integer ratio, has no valid runtime fallback. Allowing the engine to start and then failing when a player first interacts with the broken layer produces a much harder-to-diagnose problem. Startup validation fails fast with actionable messages.

### Child Grid Size Warning

When a 1000:1 ratio is configured, one composite voxel decomposing generates 10⁹ child voxels. This will exhaust memory on any current hardware. The warning prints the actual number so the game designer understands the consequence before hitting it in a playtest.

### Interactive-Layer Selection (M16, L4)

`World`'s single-layer forwarding API — `getVoxel`/`setVoxel`, dirty tracking, persistence, the collision substep scale (`voxelSizeM`), and picking — targets one **primary** layer. Which layer that is is an **explicit config choice**: the layer flagged `interactive: true` in its `LayerDef`. This makes a mid-stack playspace (the README's flying-game config — a small editable box adrift inside a huge immutable backdrop) a first-class declared target rather than a silent first-in-order pick. `LayerConfig` enforces that at most one layer is flagged (a hard error on two; see the table above). When **no** layer is flagged, `World` falls back to the pre-M16 default — the first `terminal` layer in config order, then the first layer for a stack with none — so existing single-interactive-layer configs are unchanged.

---

## 3. Voxel Modes

**Files:** `src/world/Voxel.h`, `src/world/MacroVoxel.h`

Each layer operates in one of three modes declared in the layer config. The mode determines the entire lifecycle of voxels in that layer.

### `terminal`

The leaf layer. Terminal voxels are directly player-modifiable and always persisted when modified. They do not have child layers. Most game logic operates at the terminal layer. A game with only one layer has only terminal voxels.

### `composite`

Has a composition recipe. Stays atomic (a single data record with a material type and recipe reference) until something requires its interior. On demand, runs the recipe to generate a child grid one layer down. After decomposition, child voxels are individually modifiable. See [Cascading Decomposition](#4-cascading-decomposition).

### `immutable`

No recipe, no decomposition, no player modification. Exists as geometry and collision volume only. The material type determines rendering appearance and collision response. Immutable voxels require no dirty tracking and no persistence beyond the initial generation pass. They are extremely cheap relative to composite voxels.

**Immutable voxels do not participate in upward damage propagation.** See [Upward Damage Propagation](#7-upward-damage-propagation).

### Mode Is Per-Layer, Not Per-Voxel

All voxels in a given layer share the same mode. A layer cannot contain a mix of immutable and composite voxels. This is intentional — per-voxel mode would complicate every system that queries mode and offers no design benefit.

---

## 4. Cascading Decomposition

**Files:** `src/world/MacroVoxel.cpp/.h`, `src/world/DecompositionWorker.cpp/.h`

### Why Cascading, Not Direct

If a 100km composite voxel decomposed directly into 1m terminal voxels in a single step, it would attempt to generate 10¹⁵ child voxels (100,000³). This is not a memory problem that can be solved by streaming — it is not physically possible.

Instead, each composite layer decomposes into its immediate child layer only. The chain resolves level by level as player interaction demands it:

```
100km composite  →  10km composites  (on player approach)
10km composite   →  1km composites   (on player approach)
1km composite    →  1m terminals     (on player interaction)
```

At no point does any single decomposition event generate more than `(parent_size / child_size)³` child voxels. With a 10:1 ratio, that is 1,000. With a 100:1 ratio, it is 1,000,000 — large but bounded and configurable.

### Decomposition Steps

1. Check whether a child grid already exists in the cache for this composite voxel
2. If not, dispatch to `DecompositionWorker` on a thread pool worker
3. `DecompositionWorker` runs the recipe (deterministically, using world seed + voxel position as RNG seed)
4. Child grid is inserted into the layer cache and marked clean (not dirty)
5. If the child layer is also composite, child voxels are created as atomic composite records — they do not themselves decompose until needed
6. The original interaction proceeds against the now-real child grid

### Determinism Requirement

The same recipe + (world_seed, voxel_position) inputs must always produce the same child grid output. This is what allows clean (unmodified) child chunks to be evicted from memory and regenerated on cache miss without the player noticing.

**Do not introduce non-deterministic calls into the decomposition pipeline:**
- No `rand()` or `std::rand()` — use the seeded RNG provided by `DecompositionWorker`
- No `time()` or wall-clock reads
- No iteration over `std::unordered_map` or `std::unordered_set` without a stable ordering pass first
- No thread-local state that varies between runs

### Async Pop-In

Decomposition runs on a worker thread. The main thread receives a future and can render a placeholder (the atomic composite voxel's surface material) until the child grid is ready. Pop-in is intentional and expected — do not attempt to block the main thread waiting for decomposition.

### Coarse Occupancy Must Superset Fine Occupancy

Decomposition only generates a child grid for composite voxels the coarse layer marks **present**. A composite voxel the coarse generator left empty is never decomposed, so any detail its child layer would have produced inside that volume is never generated.

Therefore **a composite layer's occupancy must be a conservative superset of its `decompose_to` child's occupancy**: every solid child voxel must fall within a solid parent. When both layers derive from a shared field (e.g. a height map), sample the coarse occupancy from the **extreme** of that field over the parent voxel's full footprint, not a single center sample — a center sample misses detail that rises into the voxel elsewhere in its footprint, which on slopes leaves holes where the fine surface crosses a coarse voxel boundary. Those holes are invisible, and because nothing decomposed there, also non-collidable (the player falls through). The layered-world `blocks_generator` had exactly this bug: it sampled the surface height at each macro voxel's center column and was fixed to take the max over the footprint.

Across a **deep chain (M10)** this invariant must hold **transitively** — every solid voxel at a fine layer falls within a solid macro voxel at *every* coarser ancestor, not just its immediate parent. For a recipe-driven stack (where child content is recipe data, not a shared analytic field) the *geometric* half is automatic: a decomposition only runs on a `present` parent and confines its child grid to that parent's subvolume, so a solid child is always within a solid parent by construction. What is **not** statically checkable is the generator-side guarantee that a coarse layer marks a macro voxel present whenever any transitive descendant would be solid — recipe output is data-dependent. This is therefore a documented **authoring contract** (a coarse recipe's occupancy must conservatively cover its descendants') backed by an optional debug-only runtime assert, rather than a config-time validation.

**Engine-derived coarse occupancy (M18.5).** When a recipe carries an **occupancy carve stage** (§6) the engine can own the coarse half outright instead of leaving it to the author. A **root** composite layer that registers an occupancy recipe but **no** layer generator has its coarse occupancy synthesized by `DecompositionManager`: for each macro it samples the recipe's carve field over the macro's footprint at child resolution, with the *same* per-macro occupancy seed (`voxel_seed_mix(decompSeed, kRecipeOccupancySalt)`) the macro's own decomposition will use, and marks the macro present iff any child cell is solid. Because it samples the identical field at the identical world positions and seed, the coarse occupancy is an **exact** superset of the macro's fine occupancy — the invariant is guaranteed by construction, not authored, so the surface lives in exactly one place (the recipe) rather than being duplicated in a hand-written coarse generator that must stay in sync. A layer that *does* register a generator keeps it (the derivation is opt-in). The full-footprint sample is `O(ratio³)` per empty macro (early-out on the first solid cell makes present macros cheap); tightening it to the field's extreme over the footprint is a noted perf follow-up.

### Engine-Owned Cascade Orchestration (M10)

Through M9 the approach-trigger / `drain` / insert / evict loop lived in each demo's main loop with a single `DecompositionState`. M10 lifts it into an engine-owned `DecompositionManager` (`src/world/`) that holds a `DecompositionState` **per composite layer** and drives the whole chain: each tick it enqueues in-radius undecomposed macro voxels whose parent is already decomposed and resident, drains worker results into the child layer, and runs the budgeted eviction pass. A decomposition whose `decompose_to` target is itself composite produces a grid of **atomic macro voxels** (step 5 above), which feed the next layer's `DecompositionState` rather than the terminal mesher. The manager owns no GPU meshes (§13) — it returns a per-tick resident/evicted diff per layer that the front-end consumes to sync its own geometry. It depends on `LODManager` for residency/budget math, which is why `LODManager` (pure headless set math) is filed in a neutral tier, not under `Renderer`.

---

## 5. Material Property System

**Files:** `include/plugin_api.h` (the `MaterialProperties` struct), `src/world/Voxel.h`, `src/simulation/RemovalModel.{h,cpp}` (the first property consumer; M8), `src/simulation/RemovalAccumulator.{h,cpp}` (the transient per-target progress state that drives held-to-mine on top of `RemovalModel`; M8). The structural `PhysicsSystem`/`PropagationSystem` consumers landed in M13.

### Design Intent

Block-type ID systems (Minecraft's approach) require every simulation system to contain a switch-case or lookup table for every block type. Adding a new block type requires modifying every system that cares about it. This does not compose.

Material properties replace the ID with a data record. Simulation systems query properties and respond to values — they do not know or care about specific material identities. A lava flow system queries `thermal_conductivity` and `porosity`. It works correctly on any material, including ones added by mods that postdate the lava system.

### Property Fields

| Field | Type | Notes |
|---|---|---|
| `density` | `float` | kg/m³; drives physics mass and structural load calculations |
| `structural_strength` | `float` | Resistance to collapse; queried by `PropagationSystem` |
| `thermal_conductivity` | `float` | W/(m·K); drives heat transfer and fire spread |
| `porosity` | `float` | 0.0–1.0; fraction of volume permeable to fluid |
| `hardness` | `float` | Relative resistance to removal/destruction; not mapped to any real-world scale. `0` = no resistance (instant removal); `< 0` = indestructible (sentinel). See the consumption contract below |
| `light_emission` | `float` | Emitted block light `[0,1]`; drives LightingSystem (§17). `0` = no emission; `1` = full-power emitter (15-voxel range) |
| `palette_index` | `uint8_t` | Index into the 256-entry visual palette; used for `.vox` compatibility |

### Palette Index and Editor Compatibility

`palette_index` is the bridge to standard voxel editors. The visual palette maps each index to color, texture, and PBR parameters. A `.vox` file stores only palette indices — importing a `.vox` file populates `palette_index` and leaves other material properties at their defaults for that palette entry. Extended properties are stored in the engine-native `.vxe` format.

### Property-Driven Behavior: the consumption contract

A simulation system reads the **target voxel's own `MaterialProperties`** and responds to the values. It never resolves properties by `material_id` and never branches on a material identity. This is the rule that makes the system compose: a system written today works on any material defined later, because it only ever asks "what are *this voxel's* properties," not "which material is this."

**Properties are read off the voxel, not looked up live.** Materials are copied into voxels by value at generation/import time (§8). A consumer therefore reads `voxel.material.<field>` directly; it does not consult the plugin material registry at simulation time. The accepted consequence: re-registering or unloading a material does **not** retroactively change voxels that already exist — their properties were frozen when they were created. The material-id registry lookup (`PluginManager::material()`, M8) exists for *tooling* — importers, UI, build-menu selection — and is deliberately off every simulation hot path.

**Worked example — voxel-removal cost (M8, `src/simulation/RemovalModel`).** The effort to remove a voxel is a pure, deterministic function of its `hardness` and an optional tool `power`:

- `hardness > 0` — work required is proportional to `hardness` (abstract units; `hardness` is relative, not a real-world scale). A tool applies `power` work-units per unit time, so `time_to_remove = hardness / power`. The mapping is linear and transparent; a game may layer its own feel curve on top of this engine primitive.
- `hardness == 0` — no resistance; the voxel removes in a single step. This is the **fail-soft default**: an unset `hardness` (the zero-initialized struct default) means "removable," and solid materials are expected to set `hardness` explicitly. The engine does not substitute a hidden default for unset values.
- `hardness < 0` — **indestructible** (sentinel; `-1.0f` by convention). The removal tool refuses to clear the voxel. This gives *per-material* indestructibility inside an otherwise-editable terminal layer, distinct from `VoxelMode::immutable`, which makes a *whole layer* non-editable. The sentinel requires no schema change — `hardness` is already a serialized `float` (§9 chunk format), so a negative value round-trips through save/load and `.vox` import unchanged.
- `power <= 0` — the tool can never remove the voxel (infinite time), regardless of `hardness`.

`RemovalModel` is pure and stateless (no `rand`/`time`/global state) and depends only on `MaterialProperties` — not on `World`, the renderer, or `PluginManager` — so it is unit-testable in isolation. The **outcome** (removed or not) is fully deterministic; only the wall-clock *time* to reach it depends on frame rate and how long the player holds the action, and that timing never touches saved world state (a voxel is binary: present or `empty()`). The per-target progress accumulator is **transient tool state**, held in the tool/demo path and never persisted; dirty tracking stays chunk-granular (§9). `on_voxel_modified` fires exactly once, at the moment the voxel is cleared.

`structural_strength` (M13 collapse) and `thermal_conductivity`/`porosity` (M14 fluid and heat) are consumers that follow this same contract: read the property off the voxel, respond to the value, stay off the material-id path.

---

## 6. Composition Recipes and Feature Generators

**Files:** `src/world/MacroVoxel.h`, `include/plugin_api.h`

### Recipe Structure

A composition recipe is a data record attached to a composite voxel type. It contains:

- **Occupancy carve stage (M18.5, optional)** — a noise/signed-distance field id + threshold that decides which child cells are *solid at all*, applied **before** the material distribution (see *Occupancy Carving* below). Absent by default, in which case the macro fills fully solid (a solid cube refines to a smaller-voxel solid cube — the pre-M18.5 behavior). Present, it lets a recipe follow a surface (a heightmap, a hollow core).
- **Material distribution spec** — a list of `(material_id, weight)` pairs plus a noise function ID and parameters controlling spatial arrangement (see *Noise Functions* below)
- **Feature generator list** — ordered list of `(feature_generator_id, parameters)` entries applied after material distribution
- **Boundary overrides** — optional per-face material-distribution specs for the macro-voxel's top, bottom, and side faces, each measured from the macro's geometric face (`MacroFace`) or, for top/bottom, from the carved surface (`Surface`, M18.5) (see *Boundary Overrides* below)
- **Seed parameters** — arbitrary key-value pairs that bias the generation of the layer below (see *Hierarchical Constraints* below)

### Occupancy Carving

By default `fillChildChunk` assigns a material to every child cell of a macro's subvolume, so a recipe refines a solid cube into a smaller-voxel solid cube — correct for fully-interior macros, useless for any macro a surface passes through. The optional **occupancy stage** (M18.5, `docs/proposals/recipe-occupancy.md`) closes that gap: it samples a registered noise field (`noise_id` + `threshold`, the same `NoiseFn` ABI a distribution uses) at each child cell's **world** center, *before* the distribution pass, and leaves the cell empty when the value is below the threshold. A plugin registers its surface once via `register_noise` (e.g. a height field whose value encodes signed distance to the surface) and the recipe names it.

The stage is **carve-only** — it may turn a cell empty but never solid — so the coarse-supersets-fine invariant (§4) holds by construction. It is seeded with a fixed salt (`kRecipeOccupancySalt`) decorrelated from the distribution and feature seed domains. A **surface occupancy field should be position-deterministic (it should ignore the per-macro seed argument)** so the carved surface is continuous across macro boundaries — a seed-dependent occupancy field would discontinue at every macro seam. Feature overlays still run after the distribution and may carve or fill further; the occupancy stage only gates the *default* fill.

### Noise Functions

Noise is a **general engine facility, not a decomposition-private detail.** The material-distribution sampler is its first consumer, but the same noise is available to any world-generation code.

A noise function is a pure, deterministic scalar field sampled at a world-space position. It is seeded by a `uint64_t` derived from `(world_seed, macro VoxelCoord)` and threaded through unchanged (no `rand`/`time`/global state). Sampling at the **world** position — not a macro-local one — makes adjacent macro voxels' child grids seamless by construction, the same property the streaming heightmap relies on.

Noise is selected **by id**, with a built-in floor and full plugin override, mirroring the built-in `.vox` import/export handlers (§10):

- The engine registers a standard set (`value`, `fbm`, `ridged`, `worley`, …) at startup as **built-in** entries (engine-owned, never torn down by a plugin unload). Implementations live in `src/world/Noise.{h,cpp}` — the engine's first in-`src` noise.
- A plugin registers its own via `register_noise(noise_id, fn)`, owner-tracked and torn down on unload like every other registry (§8). A plugin registration that collides with a built-in id **overrides** it (the same dispatch rule as importers), so a game can replace `value` wholesale or add a wholly novel `my_warped_simplex`. The built-ins are a floor, not a ceiling — a non-block game can ignore them entirely.
- A recipe references noise by id; a recipe naming an unregistered noise id (built-in or plugin) is a **startup error, not a silent skip** — the same validation rule as feature generators.

**Reuse across the plugin ABI boundary.** Engine subsystems and in-tree demos call the functions in `src/world/Noise.h` directly (in-tree consumers may reach into `src/`, §12). Out-of-tree plugins, which link zero engine symbols (§12), participate two ways: they **provide** noise through `register_noise`, and they **consume** built-in/registered noise through `resolve_noise(ctx, noise_id) -> NoiseFn` on `PluginContext` — the §12 "add a function pointer when a consumer needs it" move, landed in M16 (C2) when volumetric generators became the first non-recipe consumer. `resolve_noise` returns the registry's winning fn (a plugin `register_noise` of an id overrides the built-in floor) or `nullptr` for an unknown id, so a consumer fails loudly rather than silently mis-generating. The built-in floor (value/fbm/ridged/worley) is registered at `PluginManager` construction, so the resolver is usable from a plugin's `init` even in a host that never calls `Engine::init`. Built-in noise ignores its `user_data` argument, so the bare-`NoiseFn` accessor cannot carry a per-registration `user_data` (none of the built-ins need one). The in-tree `base-terrain` / `layered-world` generators consume the built-in `value` noise this way rather than a hand-rolled inline copy. The deterministic seed helper itself is inline in `plugin_api.h`, so it is usable everywhere with no resolver.

Noise-id → `NoiseFn` resolution happens at decomposition **job-build time on the main thread**; the resolved pointer is baked into the `DecompositionJob` so `DecompositionWorker` never consults `PluginManager` (§13 boundary).

### Feature Generators

Feature generators are plugins that receive a partially-filled child grid and a parameter set, and stamp spatial structures into it. They are applied in declaration order. Examples:

- Cave network generator: carves void regions using 3D noise
- Ore vein generator: replaces material pockets with ore materials
- Water table generator: fills below a threshold depth with fluid material
- Dungeon seed generator: places pre-authored static structures at low probability

Feature generators are identified by string name. Recipes reference them by name. If a recipe references a feature generator whose plugin is not loaded, it is a startup error (not a silent skip).

### Boundary Overrides

A recipe may attach an optional material-distribution spec to each of three faces of the macro voxel — `top`, `bottom`, and `side` (the four lateral faces share the single `side` spec). Each override carries a **depth** (default `1`) giving how many child-voxel layers inward from that face it replaces, so a multi-voxel feature such as a topsoil cap over a stone interior is expressible, not just a one-voxel skin.

By default (`BoundaryMode::MacroFace`) overrides paint the macro voxel's **geometric** outer faces — the boundary slabs of its own child grid — **not** neighbor-exposed faces: the decomposition worker stays a pure function of the single macro voxel's inputs and never samples neighboring macro-voxel occupancy (§13). Where face slabs overlap at edges and corners they are applied over the interior in the fixed order `bottom` → `side` → `top` (top wins), so a top cap reads cleanly across the rim; the order is fixed and therefore deterministic.

**Surface-relative caps (`BoundaryMode::Surface`, M18.5).** A geometric-face cap is wrong for a carved surface: a grass cap on a sloped heightmap must track the *carved* top of each column, not the macro's flat top face (which would land buried under, or floating above, the terrain). A `top` or `bottom` boundary in `Surface` mode therefore measures its `depth` inward from the **topmost (or bottommost) solid cell of each column** after the carve, in a per-column second pass in `fillChildChunk`: a solid cell whose outward neighbor is air starts a `depth`-cell band repainted with the boundary distribution. This is what lays grass-on-surface / dirt-subsoil / stone-below correctly over a slope or a radial well (the column axis follows the gravity-relative up, like the face roles). `side` has no well-defined column and is not surface-capped; without an occupancy stage the "surface" *is* the macro face, so `Surface` mode reduces to `MacroFace`. The cross-chunk limitation: a column whose surface lands exactly on an internal chunk boundary (only when a macro spans several child chunks vertically, `ratio > childChunkSize`) is conservatively under-capped by one chunk — the same exposure-at-a-seam class as the deferred neighbor-exposed boundaries above; configs with a macro one child-chunk tall (the common case) never hit it.

Exposure-aware boundaries (painting only the geometric faces that actually meet empty space) remain a deferred refinement.

### Hierarchical Constraints

A recipe's `seed_parameters` bias the generation of the layer below it. At decomposition time they are **merged into the effective parameter set** handed to the distribution sampler and to each feature generator — the same `RecipeParam` array those generators already read — so a generator reads e.g. `cave_density` without caring whether the value came from its own per-entry params or was inherited from above. **Per-entry params take precedence** over inherited `seed_parameters` on a key collision, so a feature overlay can always pin a value the parent leaves free.

Inherited `seed_parameters` are an input **carried on the `DecompositionJob`**. For M9 (single-step decomposition) the source is the decomposing layer's own recipe `seed_parameters`; this is the demo's lever — toggling one value re-runs the job and visibly changes the child grid while each value regenerates identically.

The **cross-step cascade (M10)** makes inheritance deep, and resolves it by **recomputation, not storage**. Nothing is persisted on the produced child macro voxels — a `Voxel` stays a trivially-copyable POD (a parent pointer would break RLE persistence and the plugin ABI, and create eviction-lifetime problems). Instead, when a macro voxel decomposes, its inherited param set is **reconstructed at job-build time by walking its ancestor coordinate+recipe chain** (the coordinate hierarchy the cascade already computes is the "reference" to the parent — no field is added to `Voxel`). The set has two parts:

1. **The ancestor `seed_parameters` chain** — each ancestor recipe's declared `seed_parameters`, merged root → parent. This is the explicit push-down channel ("bias toward granite" flows the whole way down, not one step).
2. **Engine-reserved, `__`-namespaced positional/material params** describing the decomposing macro voxel itself — at minimum its world position / `__altitude` and its own generated material (`__parent_material`) — injected automatically so a recipe can express position- and material-conditional rules ("no water table above 800m" reads `__altitude`; a mountain-range voxel biasing its peaks reads `__parent_material`). Namespacing keeps these facts from colliding with author params; position-dependence is **opt-in** (a recipe that ignores the reserved keys behaves identically everywhere).

Full precedence, weakest → strongest: root ancestor → … → immediate parent → this recipe's own `seed_parameters` → per-entry params (the M9 "per-entry wins" rule, with nearer/more-local context overriding farther). Because the whole set is a pure function of `(world_seed, ancestor coords, recipes)`, it is re-derived identically on a cache miss, so an evicted deep subtree regenerates byte-for-byte — the property that makes the deep cache transparent. (This per-voxel recompute is a deliberate, deterministic choice over per-layer-uniform inheritance; it is a candidate to swap back toward per-layer or to add caching if profiling ever demands it.)

This is what lets a 10km "mountain range" recipe constrain its constituent 1km "peak" recipes — e.g. "generate peaks biased toward granite, with no water table above 800m" — without either recipe needing to know about the other's implementation.

---

## 7. Upward Damage Propagation

**Files:** `src/simulation/PropagationSystem.cpp/.h` (detection), `src/simulation/PhysicsSystem.cpp/.h` (per-frame driver + event firing), `src/core/Tuning.h` (`tuning::physics` knobs). Consumers landed in M13; the design below is the M13 contract.

**Status: experimental.** The support-flood model below is proven on fixed, hand-built dioramas (`demos/13-structural-collapse`, `demos/19-multilevel-collapse`) but has a known failure mode on large streamed heightmap surfaces: the "unknown ⇒ supported" boundary rule can misread ordinary surface mining as an unsupported span, and running the detection flood every frame costs performance at that scale (it was pulled from `demos/20-mega-demo` for exactly this reason — see `docs/m18-mega-demo-metrics.md`). Treat the algorithm, its tuning knobs, and the `StructuralEvent` payload shape as **likely to change** before this is recommended for open/streamed worlds; do not build load-bearing gameplay on it without expecting to revisit it.

### Purpose

When a player mines child voxels out of a composite voxel, the composite voxel's effective `density` and `structural_strength` decrease. If enough material is removed, the composite voxel can no longer hold itself up at its own scale and collapses — potentially cascading to neighbors and stopping where it meets immovable material.

### Engine Detects, a Plugin Responds

The split is deliberate and is the central design decision of M13: **the engine only detects instability and fires an event; it never moves or clears a voxel itself.** `PropagationSystem` decides *what is unstable*; the registered structural-response **plugin** decides *what to do about it* (crumble the voxels away, spawn falling debris, something game-specific). The response plugin is therefore **mandatory** for any collapse to be visible — and that is a feature, not a gap: a project that loads no structural plugin gets Minecraft-style behavior where mining never triggers a cave-in, which is a legitimate game design rather than a degenerate case.

This makes the cascade a **feedback loop** rather than a recursive engine routine. The engine fires `on_structural_event` for each newly-unstable macro; the plugin responds by editing the world (`World::setVoxel`); those edits return through `on_voxel_modified`; the next end-of-frame pass re-evaluates and finds the next ring of newly-unstable macros; and so on until the structure is stable again. The engine stays entirely policy-free.

### The Support Model

Stability is evaluated at **macro-voxel (composite) granularity**, per the aggregation rule below — not per terminal voxel. The model is a **support-potential flood** (axis-free, so it does not bake in a "down" direction — generalized gravity is M16's concern, and M13 models *support reach*, not gravitational load):

- An **anchor** emits support potential `kAnchorPotential` (normalized to 1.0). Anchors are (1) immutable-layer voxels, and (2) the boundary of the resident/decomposed region — **a non-resident neighbor is treated as solid support**. This "unknown ⇒ supported" rule is conservative on purpose: a macro whose support could come from outside the loaded region is never declared unstable, which stops the streaming edge from spuriously collapsing and means a world with no immutable layer simply never produces false cave-ins.
- Potential floods outward through *solid* macro voxels (6-connected). Entering a macro of aggregate `structural_strength` `s` drains potential by `1 / maxSpan(s)`, where **`maxSpan(s) = clamp(s · kSupportSpanPerStrength, 0, kMaxSupportSpan)`** is the unsupported span, in macro-voxels, that material can bridge. Strength below `kMinSupportStrength` transmits no support at all (infinite cost).
- A macro is **stable iff its residual potential stays > 0.** A macro that drops to `≤ 0` is unstable and fires a structural event.

This yields the demo behaviors without special cases: strong material cantilevers farther than weak (the span formula); the weakest macro on a path drains potential fastest (weakest-link bridging falls out of the flood); removing a support lowers everything downstream of it (the cascade); and because immutable voxels are infinite-effective anchors, **propagation stops dead at an immutable layer boundary** — anything still touching bedrock is supported by definition.

### Aggregation

A decomposed macro's aggregate `structural_strength` is the **volume-weighted average of its resident child voxels' material properties**; an atomic (undecomposed) macro just uses its own block material. Aggregates are maintained **incrementally**: each `on_voxel_modified` updates the parent macro's running volume-weighted sum by the old→new delta, so a parent's aggregate is O(1) per child edit rather than an `R³` re-sum. A full recompute exists only as a bounded fallback (`kMaxAggregateRecomputesPerFrame`). Propagation walks **upward through composite layers only**.

The cascade resolves at **every composite level**, not just the immediate parent (M17, gap audit G1). The system discovers the full ancestor chain the M10 cascade computes — level 0 is the layer whose `decompose_to` is the terminal layer, and each coarser ancestor (the layer whose `decompose_to` is the prior level's composite) is the next level up. A coarse macro's aggregate is the volume-weighted average of its **child macros' aggregates**, recursively down to terminal voxels, so hollowing a terminal voxel lowers its parent macro's aggregate, which lowers its grandparent's, all the way to the root. When `recomputeAggregate` re-establishes a macro's baseline it marks that macro's parent (one level coarser) dirty, and the end-of-frame driver processes levels **fine→coarse within one tick** under the shared `tuning::physics` budgets — so a grandchild edit re-floods every ancestor level and a deep collapse (e.g. the demo's `macro→micro→grid` stack) spreads across frames with overflow carried per level, the unstable set staying byte-identical. A structural event is fired per unstable macro at each level, carrying that level's `voxel_size_m` and immediate `child_voxel_size_m`. (M13 originally resolved a single level only; the multi-level chain was the headline M17 engine item.)

### Performance

Recomputing structure inline on the modification path is forbidden; the system uses a **dirty-aggregate pattern** and defers all evaluation to a single **end-of-frame** pass, bounded by the `tuning::physics` budgets: `kMaxAggregateRecomputesPerFrame`, `kMaxStructuralEventsPerFrame` (overflow carries to the next frame, spreading a chain reaction instead of stalling one), and `kMaxSupportFloodNodes` (caps the connectivity query; with `kMaxSupportSpan` the flooded region is naturally small). The flood visits macros in deterministic sorted-coord order — no unordered iteration — so the unstable set is byte-identical across runs.

---

## 8. Plugin System

**Files:** `src/core/PluginManager.cpp/.h`, `include/plugin_api.h`

### Design: Flat Callback Registration, Not Inheritance

Plugins register named callbacks for engine hooks. They do not subclass engine types or override virtual methods. This design was chosen for three reasons:

1. **Discoverability** — the full set of things a plugin can do is visible in `plugin_api.h` as a list of registration functions. There is no base class hierarchy to trace.
2. **Isolation** — a plugin that registers a feature generator does not need to include headers for the physics system, renderer, or any other subsystem.
3. **AI agent compatibility** — flat registration functions are straightforward to understand and use correctly from a truncated context window. Deep inheritance hierarchies are not.

### Hook Registration Functions

```cpp
// World generation
void register_layer_generator(const char* layer_name, LayerGeneratorFn fn);

// Feature generators (used by composition recipes)
void register_feature_generator(const char* generator_id, FeatureGeneratorFn fn);

// Composition recipes and noise
void register_recipe(const char* layer_name, const RecipeDesc* desc);
void register_noise(const char* noise_id, NoiseFn fn);
NoiseFn resolve_noise(const char* noise_id);  // consumer accessor

// Material definitions and appearance
void register_material(const char* material_id, MaterialProperties props);
void set_palette_color(uint8_t index, uint32_t rgba);
void register_texture(const char* texture_id, const char* path);
void register_texture_data(const char* texture_id, const void* rgba, int w, int h);
void set_material_faces(uint8_t palette_index, /* top, bottom, side, tiling_factor */);

// Import/Export
void register_importer(const char* extension, ImporterFn fn);
void register_exporter(const char* extension, ExporterFn fn);

// Simulation events
void register_on_voxel_modified(OnVoxelModifiedFn fn);
void register_on_structural_event(OnStructuralEventFn fn);
void register_on_fluid_event(OnFluidEventFn fn);
void register_on_thermal_event(OnThermalEventFn fn);
void register_on_lighting_event(OnLightingEventFn fn);

// Simulation sources (plugin-registered emitters; owner-tracked)
void register_heat_source(WorldCoord pos, float rate);
void register_fluid_source(WorldCoord pos, float rate, const char* fluid_material);
void register_light_source(WorldCoord pos, float intensity);

// Layer lifecycle
void register_on_chunk_created(const char* layer_name, ChunkLifecycleFn fn);
void register_on_chunk_evicted(const char* layer_name, ChunkLifecycleFn fn);

// Per-frame tick and collision primitive
void register_on_tick(OnTickFn fn);
BodyMoveResult move_aabb(/* pos, extents, wish_delta, voxel_size */);

// Networking
void apply_edit(VoxelCoord coord, Voxel voxel);
void send_network_message(const MessageEnvelope* env);
void register_on_edit_received(OnEditReceivedFn fn);
void register_on_player_joined(OnPlayerJoinedFn fn);
void register_on_player_left(OnPlayerLeftFn fn);
void register_on_network_message(const char* channel_prefix, OnNetworkMessageFn fn);
void register_authority_policy(AuthorityPolicyFn fn);
void register_interest_filter(InterestFilterFn fn);

// Audio
void register_sound(const char* sound_id, const char* path, SoundParams params);
void register_material_sound(const char* material_id, AudioEvent event, const char* sound_id);
void play_sound(const char* sound_id, WorldCoord pos, const SoundParams* params);
void play_material_sound(AudioEvent event, uint8_t palette_index, WorldCoord pos);
AudioEmitterId create_emitter(const char* sound_id, WorldCoord pos, EmitterParams params);
void set_emitter_position(AudioEmitterId id, WorldCoord pos);
void stop_emitter(AudioEmitterId id);
```

This is the complete hook surface as of 1.0. See `include/plugin_api.h` for exact signatures and `src/plugins/ExamplePlugin` for a worked example. The `plugins/example-hooks/` plugin demonstrates every major hook type in a copy-pasteable format.

**Reference plugins (M17).** A handful of plugins under `plugins/` ship with the engine not to extend it but to spare every game from re-deriving the same boilerplate — developers use them as-is, extend, or replace. Each pairs a `plugin.cpp` with a **shared header** exposing a function-pointer `api()` table the host fills at `voxel_plugin_init` and calls at runtime (the asteroid-field geometry-sharing pattern, widened to a runtime table). `kinematic-body` (B1) drives N AABB bodies on the engine's `move_aabb` primitive via the per-frame `register_on_tick` hook. The two **input plugins** (C1, `keyboard-mouse` and `gamepad`) need *no* engine hook at all: input is read host-side before the game step, so the host drives their `update()` directly. Because the engine — not the plugin — owns the window and therefore GLFW (§9), the plugin cannot poll hardware itself; instead the host installs a tiny **`RawSource`** adapter (one-line wrappers over `glfwGetKey` / `glfwGetGamepadState` / …) and the plugin pulls raw state through it, layering on action mapping, rebindable keys, two-key axes, edge detection, mouse-look deltas, and radial stick dead-zones. No GLFW type crosses the boundary — key/button/axis indices are plain ints and the gamepad snapshot is a flat POD — so the plugins stay GLFW-free and runtime-loadable, the same library-neutrality rule the window/renderer seam enforces.

**Structural events (M13).** `register_on_structural_event` fires when `PropagationSystem` finds a composite macro voxel can no longer reach structural support (§7). The hook takes a flat-POD payload — `OnStructuralEventFn(const StructuralEvent*, void*)` — carrying the macro's world `position`, its layer voxel index (`voxel_x/voxel_y/voxel_z`, the public-ABI form of the engine's `VoxelCoord`), `layer_name`, `voxel_size_m` (its scale), `child_voxel_size_m` (the terminal child scale, so a plugin can enumerate the macro's child cells without reading back into the engine), and its post-edit `aggregate_strength` and residual `support_potential`, following the §6 `RecipeDesc` flat-struct ABI rule (no `std::` type crosses the boundary; field order is append-only). One event describes one macro; the engine only detects and reports, and the registered plugin owns the response (clearing voxels, spawning debris). With no structural-response plugin loaded, mining never triggers a collapse.

**Fluid and thermal events (M14).** `register_on_fluid_event` / `register_on_thermal_event` fire when an engine-owned field overlay crosses a reporting threshold (§17) — a fluid cell reaches saturation, a thermal cell crosses a configured temperature. Each takes a flat-POD payload (`OnFluidEventFn(const FluidEvent*, void*)` / `OnThermalEventFn(const ThermalFieldEvent*, void*)`) carrying the cell coord and the field value at the crossing, following the same append-only, no-`std::` ABI rule as `StructuralEvent`. As with structural events the engine only *detects and reports*: it writes its own overlays but never a voxel. The **mandatory `flow` response plugin** turns a `FluidEvent` into real fluid geometry via the public edit path below — with no fluid plugin loaded, fluid simulates as a field but never becomes a voxel (the legitimate "no fluid geometry" config). `register_heat_source` / `register_fluid_source` are the inbound direction: plugin-registered emitters the engine injects into the overlays each tick, owner-tracked and torn down on unload like every other registry.

**The public edit path (M13).** A structural-response plugin acts on an event through `ctx.apply_edit(ctx, WorldCoord, const Voxel*)` — the *public edit path*. It routes to the engine's single edit choke point (`NetworkManager::applyEdit` → `World::setVoxel` → `on_voxel_modified`), the same path every player and replicated network edit takes, so a plugin edit is indistinguishable from a player edit and re-enters detection through `on_voxel_modified`. This is what closes the cascade as a **feedback loop** rather than a recursive engine routine (§7): `PhysicsSystem` fires events → the plugin edits via `apply_edit` → the macro re-dirties → the next end-of-frame `PhysicsSystem::tick` re-evaluates the next ring, terminating at an anchor. `PluginManager` stores the handler only; `NetworkManager::init` installs it (mirroring `send_network_message`). With no edit handler installed (a host with no `NetworkManager`), `apply_edit` is a fail-soft no-op — reinforcing the engine-never-writes default. `PropagationSystem` itself observes edits at the choke point by riding an *engine-owned* `on_voxel_modified` hook (`PluginManager::registerEngineVoxelModifiedHook`, owner `kBuiltinOwnerId`, registered by `PhysicsSystem`), so detection sees every committed edit without `NetworkManager` ever depending on the simulation tier (§13).

### Plugin Load Order

Plugins are loaded in declaration order from the project config. Hook registrations from earlier plugins are visible to later plugins. If two plugins register the same feature generator ID, the second registration overwrites the first with a logged warning.

### Loading, Unloading, and Registry Ownership

`loadPlugin` resolves `voxel_plugin_init` from the shared library, calls it with a fresh `PluginContext`, and returns a `PluginId` (or `kInvalidPluginId` on any failure: cannot-open, missing symbol, or non-zero init). `wireInPlugin` does the same for a plugin compiled into the executable, with no library handle. The example plugin (`src/plugins/ExamplePlugin`) is wired in for the M3 demo; the `plugins/base-terrain` and `plugins/water` libraries are the disk-loaded form used by the M4 demo.

Every registry record carries the **owner** `PluginId` of the plugin that registered it. The owner is set on `PluginManager` around each `init` call, so the `register_*` lambdas tag whatever they append. This is what makes per-plugin unload possible without an API change — the owner is engine-internal and never exposed through `PluginContext`.

`unloadPlugin(id)` erases every registry entry owned by `id` **before** closing the library handle. Ordering is a correctness requirement, not a nicety: a registry still holding a function pointer into a `dlclose`d library would dangle the moment a subsystem invoked it. The same rollback runs when an `init` returns non-zero (it may have registered callbacks before failing) — partial registrations are removed before the library is closed.

**Materials are copied into voxels by value** (`Voxel` holds a `MaterialProperties`, not a material-id reference). Unregistering a material therefore does not retroactively change voxels that already exist; the material registry is consulted only at generation time. A consumer that wants an unload to be *visible* must regenerate the affected chunks — the M4 demo does this by dropping and re-streaming resident chunks whenever the plugin set changes.

### Feature Generators in the Terminal-Layer Path

Feature generators (`register_feature_generator`) post-process an already-filled voxel grid. In the full design they are referenced by composition recipes (§6); for the terminal-layer M4 demo they are applied directly: after the base layer generator fills a chunk, each registered feature generator runs over it in registration order. The `water` plugin uses this to flood empty voxels up to a fixed sea level — additive, self-contained, and cleanly removable, since dropping the plugin removes its feature generator from the pass.

---

## 9. Renderer and LOD

**Files:** `include/renderer/Renderer.h` (abstract interface), `include/renderer/Frustum.h` (view-frustum culling), `include/renderer/RendererFactory.h` + `src/renderer/RendererFactory.cpp` (the bgfx-free creation seam), `src/renderer/BgfxRenderer.cpp/.h`, `src/renderer/LODManager.cpp/.h`, `src/platform/Window.cpp/.h`, `include/platform/NativeWindowHandles.h`

### Windowing and Surface

The window is owned by a separate platform layer (`src/platform/Window`, backed by GLFW), not by the renderer. The window creates no graphics context of its own (`GLFW_NO_API`) — bgfx owns the device and renders into the window's native handle.

The seam between the two is `platform::NativeWindowHandles`, a small struct carrying the native window/display pointers (and a Wayland flag). The window layer produces it; the renderer consumes it to populate `bgfx::PlatformData` in `Renderer::initialize`. This keeps the dependency one-directional and library-neutral: **the window layer never includes bgfx, and the renderer never includes GLFW.** Do not collapse these — a renderer that pulls in GLFW, or a window that pulls in bgfx, defeats the separation that lets either backend be swapped.

bgfx is initialized in single-threaded mode (a `bgfx::renderFrame()` call precedes `bgfx::init`), so device and window calls stay on the main thread — required on macOS and simpler everywhere.

**Linux:** X11 is requested explicitly for now (`GLFW_PLATFORM_X11`); native-handle wiring for Wayland is a planned follow-up. On a Wayland session the window runs through XWayland until then.

### Layer-Aware Rendering

Each layer has its own view distance and chunk budget, configured per-layer. A 100km layer needs very few visible chunks at any one time (the player can see maybe 3–4 in any direction). A 1m terminal layer needs many small chunks in a tight radius. Sharing a single view-distance setting across all layers would either waste memory on macro-layers or starve the terminal layer.

`LODManager` maintains per-layer chunk visibility sets and evicts chunks that fall outside the view distance budget.

**Streaming volume (M16, L1).** Residency is decided by a per-layer, camera-centered `StreamingVolume` (`src/world/StreamingVolume.h`) with **no privileged axis**, replacing the old hard-coded "XZ-Chebyshev disc × absolute-Y band" footprint that silently bottomed a deep-dig world out on empty space once it left the configured band. The volume has a selectable shape set on the `LayerDef` (`streaming_volume.shape`):

- `box` — an isotropic 3D Chebyshev cube of radius `view_distance_chunks`, camera-relative in **every** axis. This is the default and reproduces the pre-M16 footprint byte-for-byte, so existing configs are unchanged. A deep descent now streams downward with the camera. The legacy vertical band survives as a box-only convenience (`LODManager::setVerticalBand`) for heightmap worlds that only populate a few chunk-Y indices.
- `sphere` — an isotropic Euclidean ball of radius `view_distance_chunks` (excludes the box corners), for a space world surrounded in every direction.
- `shell` — a thin Euclidean band `[view_distance − shell_thickness_chunks, view_distance]`, for a backdrop that need only be resident at range (a flying game, a far skybox-like immutable layer).

`LODManager::desiredChunks` / `withinViewDistance` / `shouldEvict` are all queries against this volume centered on the live camera chunk; eviction tests the volume grown by the hysteresis margin (`StreamingVolume::expandedBy`) so a chunk crossing the load boundary is not immediately thrashed. Because `DecompositionManager` already drives residency through `LODManager`, the working-set shape follows automatically for composite, terminal, and (M16) immutable layers, each still bounded by its own `resident_chunk_budget`.

### Decomposed-Mesh Budget

Each resident chunk mesh costs GPU buffer handles — in the bgfx path, one static vertex buffer and one static index buffer — and **bgfx caps static vertex and index buffers at 4096 each** (`BGFX_CONFIG_MAX_{VERTEX,INDEX}_BUFFERS`). Decomposition multiplies chunk count: one coarse voxel becomes up to `(parent_size / child_size)³` worth of fine chunks. So **decomposed child meshes must be bounded on a tight radius around the camera, not merely retained out to the composite layer's (much larger) view distance** — otherwise flying across terrain accumulates child meshes without limit.

The failure mode when the ceiling is exceeded is silent and nasty: `bgfx::createVertexBuffer`/`createIndexBuffer` return invalid handles, but `ChunkMesh::build` does not check validity and `ChunkMesh::empty()` is face-count-based, so the chunk yields a *non-empty* mesh holding *invalid* handles. The renderer queues it, then skips it at submit (the `bgfx::isValid` guard in `BgfxRenderer::render`). The chunk's voxel data is still resident in its layer, so **it collides while rendering nothing — invisible but solid — and never recovers**, because nothing rebuilds it. Bound the resident decomposed set so the handle count stays under the ceiling: demo 05 reverts terrain past a keep radius back to its coarse block, freeing the buffers and letting it re-decompose on re-approach. Durable lifts for larger radii are merging chunk meshes (fewer, larger buffers) or raising the bgfx config caps.

### Floating Origin

The renderer maintains the camera's current position as a `WorldCoord`. Before any geometry is submitted to the GPU, vertex positions are converted to camera-local space via `WorldCoord::toLocalFloat(camera_position)`. The result is a `glm::vec3` of small magnitude that fits safely in 32-bit float precision.

**Do not submit `WorldCoord` values directly to the GPU.** Always go through `toLocalFloat`.

### Camera Orientation (M17, surface-normal up)

The view matrix is built from the camera's `pitch`/`yaw`/`roll` interpreted in a frame whose **up axis is a settable world-space vector** (`Renderer::setCameraUp`, default `(0,1,0)`). This is the visual counterpart to M16's gravity-relative grounding (L2/L7): once "down" can point any direction, the camera can align its up-axis to the local surface normal (the local `-gravityAt` up), so a player standing on the +X face of an asteroid sees a **level horizon** instead of a tilted one.

The shared `cameraBasis()` (`include/renderer/CameraBasis.h`) builds the canonical +Y-up forward/right/up from pitch/yaw (and roll about the forward axis), then rotates that whole frame by the rotation mapping +Y onto the supplied up. **With the default up and zero roll the rotation is the identity, returning the canonical formulas unchanged** — so every existing scene's view matrix is byte-for-byte the pre-M17 one (`BgfxRenderer::render` keeps the historical float path verbatim for that case). The same basis feeds view-frustum culling (`Frustum::update` takes the up vector and roll) so the culling planes rotate with the view rather than staying Y-up. A game on a many-bodied world calls `setCameraUp(-gravityDir)` each frame; in zero-g it forwards the previous up (a zero vector is ignored) so the basis never degenerates.

**Animating the reorientation is the game's job, not the renderer's.** The renderer is told the exact up each frame and renders it; it owns no animation clock. To *ease* an up change (landing on a surface, crossing between gravity bodies) instead of snapping, a game smooths the vector host-side and feeds the smoothed value to **both** `cameraBasis` and `setCameraUp` — keeping the rendered view and the game's picking/raycast locked together through the tween (if the renderer animated internally, the host would no longer know the displayed up and its raycast would desync). `cameraBasis.h` supplies a pure, stateless step for this — `rotateUpToward(current, target, maxRadians)` rotates one unit up toward another by a capped angle (settling exactly on arrival, with an antipodal-axis fallback) — but the *rate* and the per-frame state stay the game's policy. `demos/17-asteroid-belt-miner` uses it (turn rate `kUpTurnRate`) so the suit's horizon eases level over a few tenths of a second.

The directional **shade ramp** in the mesher is a third consumer of the up vector. `buildChunkMeshData`'s fake-lighting ramp (top brightest, bottom darkest, sides between) resolves each face's role through `axisrole::roleOf` against the same gravity vector that drives the textured face roles (M16 G1) and recipe boundaries (G2), so the lit relief follows local "up" on an off-axis body instead of always lighting +Y (M16 G3). Under the default −Y gravity `roleOf` is the identity, so the ramp is byte-identical to the historical +Y-up shading.

### Distance Obscurance (M17, atmospheric fog)

The decomposition cascade stages coarse→fine geometry at **decoupled distances** (§4; the Asteroid belt miner refines a body's 4 m silhouette at 280 m and its 1 m mineable grid only within 90 m). Drawn at full clarity to the far clip, that transition — and the chunk-load boundary at the view-distance edge — is baldly visible: detail **snaps** into place rather than resolving. Distance-obscurance fog is the depth cue that hides the pop: geometry emerges out of murk as it refines. Tuning the obscurance band to sit just inside each layer's decompose distance conceals the transition entirely; it complements, not replaces, the decompose-distance tuning.

Like gravity (§18) and authority (§15), fog is a **policy, not a baked engine force**. The renderer owns only the *mechanism*: `Renderer::setFog(FogParams)` (`include/renderer/Fog.h`) takes a fog color and a near/far band with a max density, and the chunk fragment shader (`shaders/fs_voxel.sc`) blends each shaded fragment toward the fog color by a linear near→far ramp clamped to `[0,1]` and scaled by density. The vertex shader outputs the view-space distance (`length(mul(u_modelView, position))`) the ramp reads; `gl_Position` is computed exactly as before, so the geometry path is unchanged. Only RGB is fogged — the fragment's alpha (translucency) is preserved.

The fog color, the band, and how (or whether) they **animate** are supplied per-frame by the game, typically from a content plugin. Two reference suppliers ship (each a pure-policy plugin that fills a shared `api()` table at init and registers no engine hooks, like the input plugins): **atmospheric-mist** (`plugins/atmospheric-mist`) — a dust haze fading to the sky color whose peak density slowly breathes, for open fields; and **range-attenuation** (`plugins/range-attenuation`) — a flickering lit radius fading to near-black, the "you can only see so far in here" cave/torch look, riding the *same* fog mechanism with a dark target color and a band equal to the light's reach. The host queries `api().sample(t)` each frame and forwards the result to `setFog`. `demos/17-asteroid-belt-miner` wires up atmospheric-mist with its band tuned to the macro decompose distance (and a toggle key — `M` — to A/B the LOD pop with and without the haze).

**Fog color must match the background.** Fog fades geometry toward the fog color, so for far geometry to dissolve *seamlessly* into the background — rather than leaving a halo where a silhouette meets the cleared void at the far plane — the fog color and the framebuffer clear color must be the same. `Renderer::setClearColor(rgb)` is the companion seam: a game pairs `setClearColor(c)` with a fog color of `c`. The default clear (`0x303030`) is byte-identical to the historical one, and the renderer re-asserts it each frame so a runtime change (e.g. toggling the haze) takes effect immediately.

**The default (`FogParams.density == 0`) disables fog**, so every existing scene renders byte-identically: the shader's `mix()` collapses to the un-fogged fragment exactly when the factor is 0, and the uniforms default to zero so a renderer whose host never calls `setFog` applies no fog. `fogFactor()` in `Fog.h` is the CPU mirror of the shader's curve (used by tests and reasoning about the band).

### Opaque and Translucent Passes

A material is translucent when its **palette entry's alpha byte is below `0xff`** (`palette::isTranslucent`); water is the first such entry. This single bit of data drives the whole transparency path without any per-material renderer code:

- **Mesh build** (`buildChunkMeshData`) splits a chunk's faces into two index batches over one shared vertex buffer — opaque and translucent — by the voxel's palette alpha. Translucent voxels additionally **do not emit faces on a chunk border**: water continues across the seam, and emitting both chunks' boundary faces would double-blend into a visible grid of walls. (Opaque border faces are still always emitted, since there is no cross-chunk neighbor lookup and they are hidden back-to-back.) The mesher itself owns no buffers — it appends into caller-supplied vectors and `.clear()`s them at entry — so `ChunkMesh::build` keeps **`thread_local` scratch** for the vertex and index outputs and reuses it across every chunk it meshes; the retained capacity means steady-state meshing never reallocates (the M17 profiling pass measured this at ~25–46% of CPU mesh-build time, `docs/m17-performance-profiling.md`), and the bgfx upload copies out of the scratch so reuse is sound.
- **Submission** uses two bgfx views sharing the camera transform and the back buffer's depth: view 0 draws the opaque batch with `BGFX_STATE_DEFAULT` (depth write on); view 1 draws the translucent batch after it with alpha blend, depth **test** on but depth **write off**, and **no backface cull** (so a water surface reads from both above and below). View 1 does not clear, so it composites over view 0's color and tests against its depth. Chunks are sorted back-to-front by squared distance from the camera before submission so that overlapping translucent faces alpha-blend in the correct order.

This keeps transparency a property of the data (palette alpha), not of any subsystem's special-casing: a plugin makes a material translucent simply by pointing it at a translucent palette index. Interior faces between like voxels are still culled, so a filled body of water renders only its outer shell.

### View-Frustum Culling

`BgfxRenderer::render()` performs conservative view-frustum culling before sorting or submitting chunks to the GPU. Each frame the renderer builds a `Frustum` (`include/renderer/Frustum.h`) from the current camera position, orientation, aspect ratio, vertical FOV (60°), and far clip distance. Pending chunks are tested against the frustum's five planes (near, far, left, right, top/bottom) using a bounding-sphere check — each chunk's sphere is centered at its world-space midpoint with radius `chunkWorldSize × √3/2` (the half-diagonal). Chunks whose sphere lies entirely outside any plane are discarded via `std::remove_if` before the back-to-front sort.

The test is conservative: it never culls geometry the GPU would actually draw, but may submit chunks that are marginally outside the frustum (sphere vs. box overestimation). `renderChunk()` accepts an optional `chunkSizeVoxels` parameter (default 32) alongside `voxelSizeM` so the renderer can compute the correct bounding sphere for any layer scale.

### Texture Atlas

Voxel faces can be textured, not only flat-colored (M15; `docs/m15-textured-voxels-audit.md`). The mesh vertex carries a `TexCoord0` (a tile-local `u,v`) and a `TexCoord1` (the bound tile's atlas sub-rect `u0,v0,u1,v1`) alongside its packed color (`MeshVertex`/`VoxelVertex` stay byte-compatible — the same `bgfx::VertexLayout` uploads both), and `fs_voxel.sc` wraps `fract(TexCoord0)` into that sub-rect and modulates the sampled texel by the vertex color, so the existing per-face shade and palette-alpha translucency still apply. The tile-local UV is scaled by `face_world_size × tiling_factor`, so a material **tiles at a fixed world density** rather than stretching — the `fract` wrap, against the atlas sub-rect, is how an atlas tile *repeats* (hardware `REPEAT` cannot wrap a sub-rectangle); the atlas is point-sampled, so a wrapped coordinate lands on the tile's own edge texel with no neighbor bleed. **The default is a 1×1 white tile** the renderer owns and binds before every submit: with no content atlas installed, UV `(0,0)`, and the full-atlas sub-rect `(0,0,1,1)` on every vertex, the sample returns white and colored worlds render byte-identically to the pre-texture pipeline. A content atlas is installed via `BgfxRenderer::setAtlas` (the renderer references it but does not own it).

Like color (the palette side table), **texture is keyed by material, not stored on `Voxel`** — there is no per-voxel texture field, so the POD, the memcmp determinism padding, RLE persistence, and the plugin ABI are untouched. Images enter through `register_texture` (a path) or `register_texture_data` (in-memory encoded bytes, e.g. a Blockbench `.bbmodel`'s embedded base64 texture) — both owner-tracked in `PluginManager`, the audio-registry pattern; `TextureManager` (`src/renderer/`) decodes each via bimg, packs them with the headless `TextureAtlasData` shelf-packer, uploads one `bgfx::createTexture2D` atlas, and exposes each tile's UV sub-rect. On plugin unload the registry is pruned and the atlas **rebuilt from the survivors**, so a plugin's tiles vanish with it (the §8 teardown contract).

The `(palette_index, face) → tile` binding the mesh builder consults lives in the headless `materialfaces` table (`src/renderer/MaterialFaces.h`), the side-table analog of `palette`: `set_material_faces` binds a material's top/bottom/side to `texture_id`s plus a per-material tiling factor, a **global runtime binding echoing `set_palette_color`** (not owner-tracked). Resolution is two-step and joined at query time so the mesh builder stays GPU-free — the binding holds `texture_id`s, and `TextureManager` republishes each atlas pack's resolved sub-rects into the same table; a binding therefore falls back to the white tile automatically once its texture's owner unloads and the atlas rebuilds without it, needing no second teardown path. The Blockbench importer (`plugins/blockbench`) is the first content producer: on the `register_importer` seam it decodes a `.bbmodel`'s embedded textures into the atlas, binds a material per element via `set_material_faces`, and fills the voxel grid — one-way import (exporter/round-trip and per-face sub-UV sheets are follow-ons).

### Immutable Layer Rendering

Immutable layer voxels are rendered as static geometry with no editing overhead — they never participate in the dirty/persist cycle. Since M16 (L5) their **meshes are streamed under the layer volume** rather than generated once and fully retained: `DecompositionManager` brings immutable layers into the same residency cycle as composite/terminal layers, loading/evicting their chunks by the per-layer `StreamingVolume` + `resident_chunk_budget` and reporting each as an `isImmutable` `LayerTickDiff`. Because an immutable chunk has no save path, eviction simply drops it; it regenerates byte-identically from seed on re-entry (it is a pure function of `(world_seed, ChunkCoord)`). This lets a vast sparse backdrop be configured as a thin `shell` volume that is only ever resident at range, instead of forcing the whole backdrop into memory at world load.

### HUD / 2D Overlay Seam

The renderer exposes a deliberately minimal **2D overlay seam** for game HUDs, drawn on top of the 3D scene through bgfx's built-in 8×16 debug-text cells (the same mechanism as the crosshair). The engine does **not** ship a UI framework — a general retained-mode UI is enormous scope and unlikely to fit most games (M17 sanity-check C2). Instead `BgfxRenderer` offers an **immediate-mode cell-grid draw list** (`hudClear` / `hudText` / `hudFill` / `hudCells`, with `hudCols`/`hudRows` reporting the live grid): the game rebuilds the overlay each frame from its own state, and the renderer rasterizes it into the shared debug-text pass and clears it. Cells carry a 4-bit foreground + 4-bit background color (`hud::attr`). This is enough to compose a real HUD — health bars (`hudFill`), an inventory hotbar (`hudText`), a minimap (`hudCells`, a blitted block of colored cells) — without any per-game renderer change, and it keeps the overlay policy (layout, what to show) on the game side where it belongs. The reference is `demos/18-hud-and-controls`; the older single-line `setHudText` persists for simple status text. A game that wants richer 2D (textured quads, vector text) supplies its own pass — the seam is intentionally a floor, not a ceiling.

### Renderer Backend Targets

bgfx selects its graphics backend at compile time. The targeted backends per platform are:

- **Windows** — Vulkan (preferred), Direct3D 12 as fallback
- **Linux** — Vulkan
- **macOS** — Metal

Do not write renderer code that assumes a specific underlying API. All GPU interaction must go through bgfx's abstraction layer. In particular: do not use Vulkan-specific extensions, Direct3D-specific resource hints, or Metal-specific texture formats directly — if something requires platform-specific behavior, it belongs behind a bgfx capability query (`bgfx::getCaps()`), not a compile-time `#ifdef`.

The compile-time backend selection also means the project ships separate binaries per platform rather than a single binary that selects at runtime. This is a known bgfx tradeoff and an acceptable one for this project's scope.

### Shaders

Shaders are authored in bgfx's `.sc` dialect (`shaders/*.sc`, with `varying.def.sc` declaring the vertex attributes and interpolants) and compiled by bgfx's `shaderc` into per-backend bytecode, emitted as embeddable C headers. `BgfxRenderer` includes the per-backend headers and selects the variant matching the live backend at runtime via `bgfx::createEmbeddedShader`.

**Why the bytecode is precompiled and committed, not built in CI.** bgfx's `shaderc` unconditionally links `tint`/Dawn (bgfx's WebGPU backend) — thousands of translation units that exhaust the CI runners' time/memory. Building it on every CI run (or every clean build) is impractical. So the compiled headers are **committed under `shaders/generated/`**, and the shader toolchain is built only on demand:

- **Normal builds and CI** consume the committed headers and need no toolchain (`VOXEL_BUILD_SHADERS=OFF`, the default; bgfx tools are not built).
- **Regenerating** after editing a shader is explicit and local:
  ```
  cmake -B build -DVOXEL_BUILD_SHADERS=ON
  cmake --build build --target voxel-shaders   # then commit shaders/generated/
  ```
  `shaderc` cross-compiles all backends from one host, so the committed set is complete on any developer machine.

The committed profile set covers the backends bgfx auto-selects: SPIR-V (Vulkan), GLSL and ESSL (OpenGL/ES), DXBC (Direct3D11), and Metal. Two are intentionally omitted, both documented follow-ups:

- **Direct3D12 (DXIL):** shaderc's DXC path rejects its own generated varying-initialization under `--werror`. bgfx prefers Direct3D11 on Windows regardless, so D3D12 is deferred rather than worked around.
- **WebGPU (WGSL):** not a current target.

When adding or changing a shader, keep three things in sync, or the build breaks: the regeneration profile rows in `CMakeLists.txt`, the committed headers under `shaders/generated/`, and the embedded-shader backend matrix in `BgfxRenderer.cpp` (the `BGFX_PLATFORM_SUPPORTS_*` overrides and per-profile `#include`s). A referenced-but-missing profile is a build error; a committed-but-unreferenced one is dead weight.

---

## 10. Import/Export and Editor Interoperability

**Files:** `src/io/VoxImporter.cpp/.h`, `src/io/VoxExporter.cpp/.h`

### Interoperability Philosophy

Standard voxel editors (MagicaVoxel, Qubicle, Goxel) work with single-scale, palette-indexed cubic grids. The engine supports them for the common case — single-layer, palette-material content — and progressively diverges only when a game maker opts into extended features.

### `.vox` Import

A `.vox` file is always imported into a specific layer at a specific world-space anchor. The importer populates `palette_index` for each voxel and installs the file's authored RGBA colors into the engine's visual palette for the indices the model uses, so imported voxels render with — and re-export to — the colors they were drawn with (the palette is the index→color table in `src/renderer/Palette.h`; export reads it back). Other material *properties* are initialized from the palette entry's default property set; the engine does not infer material properties from color values — only the visual color is carried across.

`.vox` volumes larger than 256³ are assembled from multiple objects using the anchor positions encoded in the file format.

### `.vxe` Format (Engine-Native)

`.vxe` is the engine's native format for content that cannot be represented in `.vox`:
- Multi-layer anchored content
- Voxels with extended material properties
- Composition recipe references

`.vxe` files may include a `.vox` sidecar for the palette/visual layer, allowing standard editors to open and edit the visual appearance while the engine retains the extended data.

### Export Fallback

If a plugin adds non-standard voxel data but does not register an exporter, the engine exports the `palette_index` values to `.vox` and logs a warning that extended data was dropped. This is lossy but produces a valid file.

---

## 11. Persistence and Dirty Tracking

### Dirty Tracking Granularity

Dirty tracking is at **chunk granularity within a composite voxel**, not per-voxel. A chunk is a fixed-size subvolume of a layer; its side length is a per-layer config value (`chunk_size_voxels` in `LayerConfig`, default 32, with shipped configs setting it as low as 4 for finely decomposed layers). The chunk grid is `chunk_size_voxels³`. When any voxel in a chunk is modified, the chunk is marked dirty and scheduled for persistence.

Per-voxel dirty tracking was rejected because a single large edit (removing many voxels at once) could mark millions of individual voxels dirty, producing save file write amplification that makes autosave impractical.

### What Gets Persisted

- All dirty chunks (player-modified)
- Immutable voxels: nothing (regenerated from seed on load)
- Clean (unmodified) recipe-generated chunks: nothing (regenerated on cache miss)
- Composite voxel decomposition state: **not persisted (M10 decision)**. Because decomposition is deterministic, re-decomposing on load reproduces the identical clean state, and any dirty descendant reloads from its chunk file as its layer re-streams. Persisting "which composites are decomposed" to disk would only avoid redundant re-decomposition work on load — a load-time optimization deferred to a later save-game milestone, not a correctness requirement.

### Cache Eviction

Clean chunks can be evicted from memory when they fall outside the LOD view distance budget. On cache miss (player re-approaches), they are regenerated deterministically from the recipe. This is transparent to the player only if decomposition is deterministic — see [Cascading Decomposition](#4-cascading-decomposition).

Dirty chunks are never evicted from disk, but may be evicted from the in-memory cache and reloaded on demand. A dirty chunk is always **saved before its in-RAM drop** (never silently discarded).

### Save Format and Versioning

Dirty chunks are written as per-chunk `.vxc` files (`persistence::WorldSave`, `src/io/ChunkPersistence.cpp`): a `VXCK` header carrying a `uint32` format version + the world identity (`voxel_size_m`, `chunk_size_voxels`) + the chunk coord, then a deduplicated material palette and a run-length encoding of the per-voxel index stream. This is the engine's own little-endian save format, distinct from the portable `.vox`/`.qb` interop path (§9). The reader requires an **exact version match** — an older or newer file decodes to `nullptr`, which the world treats as a cache miss and regenerates from the recipe (safe but edit-dropping). The full versioning contract — what bumps the version, what the engine does on a mismatch, and the forward-migration path consumers should plan around past 1.0 — is documented in [`save-format-versioning.md`](save-format-versioning.md).

**Memory budget across a deep stack (M10).** Distance + hysteresis is the primary eviction signal, but a deep chain holds several layers resident at once, so each composite/terminal layer also carries a **per-layer resident-chunk cap** (a `LayerDef` field). When a layer exceeds its cap the `DecompositionManager` evicts its **farthest-first clean** chunks to fit, pinning near and dirty chunks. (A global estimated-byte budget is a deferred outer backstop — farthest-first across all layers — that layers over the per-layer caps if a lopsided-density config needs it; per-layer chunk count is the deliberately simple starting metric.) Eviction is **cascading**: evicting a parent macro voxel back to atomic evicts every decomposed descendant across all deeper layers in one consistent pass — the inverse of the level-by-level decomposition walk — clearing each layer's `DecompositionState` entry with no orphaned resident child grids.

---

## 12. Build, Packaging, and the Engine Library Boundary

**Files:** `CMakeLists.txt`, `include/`, `src/`, `demos/`, `tests/`

### The Engine Is a Library; Front-Ends Are Separate Executables

The engine builds as a library target (`voxel-engine`) compiled from everything under `src/`. It contains no `main()`. Games, demos, development tools, and the test suite are **separate executable targets that link the library**:

```
voxel-engine        (library)     ← all of src/**
demos/<NN-name>     (executable)  ← progressive reference examples; each links voxel-engine
voxel-engine-tests  (executable)  ← links voxel-engine + GoogleTest
plugins/*           (shared lib)  ← built independently, loaded at runtime
```

A plugin-based engine is meant to host many front-ends — the per-milestone demos in the README, and eventually real games — so the engine cannot itself be the executable. The test suite is a concrete forcing function: tests must link engine code *without* a `main()`, which is impossible if the engine and `main()` are one target.

In-tree executables (the demos and tests in this repository) are privileged consumers: they may reach into `src/` for engine internals. Out-of-tree consumers (third-party games and plugins) see only the public API in `include/`.

### Static by Default, Shared by Flag

The library builds **static by default** and honours CMake's `BUILD_SHARED_LIBS` — `-DBUILD_SHARED_LIBS=ON` produces a shared library instead, with no source changes. Static is the right default during active development: there are no export-macro or symbol-visibility annotations to maintain across a still-churning API, no runtime library deployment or path management, and link-time iteration stays fast.

A shared build becomes worthwhile only when shipping a prebuilt engine SDK that third parties link without recompiling, or when many executables should share a single engine binary. At that point, add `__declspec(dllexport/import)` / visibility-attribute export macros to the public headers — but **not before**, and never as a workaround for something the static build handles fine.

### Public / Private Header Boundary

The library's include directories encode the API surface:

- **`include/` is `PUBLIC`** — the committed public API. It propagates to every consumer that links the library. It holds `WorldCoord.h`, `plugin_api.h`, the consumer front-end types (`core/Engine.h`, `core/LayerConfig.h`), and the renderer seam (`renderer/Renderer.h` + `renderer/RendererFactory.h`, with `platform/NativeWindowHandles.h`). Subdirectories under `include/` mirror the `src/` layout, so an include path like `"core/Engine.h"` resolves the same for an in-tree demo and an out-of-tree game.
- **`src/` is `PRIVATE`** — engine internals, on the path only for the library's own compilation.

Dependency visibility mirrors this boundary, and is itself a load-bearing decision:

- **`glm` is `PUBLIC`** — it is the only third-party type intentionally exposed in a public header (`WorldCoord` wraps `glm::dvec3`).
- **`bgfx` / `bx` / `bimg` and `yaml-cpp` are `PRIVATE`** — implementation details that must not appear in any public header. `dl` is carried portably via `${CMAKE_DL_LIBS}`.

**Rule:** do not introduce a private dependency's types into a public header. In particular, keep `bgfx` out of `include/` — rendering is exposed to consumers through the abstract `Renderer` interface, never through concrete bgfx handles. Leaking bgfx into the public API would also make a future shared build far harder (it would drag the graphics ABI across the library boundary).

### The Plugin ABI Is Independent of the Packaging Choice

This is the invariant that makes static-vs-shared a free decision rather than a constraint. Plugins receive a **function-pointer table** (`PluginContext`, see [Plugin System](#8-plugin-system)) and link **zero** engine symbols. A plugin's only exported symbol is `voxel_plugin_init`, which the engine resolves at runtime via `dlopen`/`dlsym` (or `LoadLibrary`/`GetProcAddress`). Whether the engine is a static library baked into an executable or a standalone shared library, plugins load and call back identically.

**Corollary:** if a plugin needs a capability the engine does not yet expose, the fix is to **add a function pointer to `PluginContext`** — never to export engine symbols, link the engine into the plugin, or force a shared build.

### Toolchain Notes

- **C++20 is required** (bgfx's `bx` uses designated initializers in its SIMD headers).
- Third-party dependencies are fetched with CMake `FetchContent` and pinned (bgfx.cmake to a release tag) for reproducible builds. Some pinned deps predate modern CMake's policy floor, which `CMAKE_POLICY_VERSION_MINIMUM` accommodates.

### The Public-Header Surface (finalized in M17)

The core consumer-facing types now live in `include/` and are the **committed** public API. M17 promoted the clean front-end leaves — `Engine` (lifecycle / entry point) and `LayerConfig` (the layer-stack DSL) — and the renderer's abstract `Renderer` interface, each of which depends only on already-public types (`WorldCoord`, `NativeWindowHandles`) and so promotes without dragging engine internals across the boundary. The renderer is exposed behind a **creation factory** (`createRenderer()` in `renderer/RendererFactory.h`): a game asks the factory for a `std::unique_ptr<Renderer>` and drives it through the abstract interface alone, so bgfx — a `PRIVATE` dependency whose handles must never appear in a public header — stays entirely inside `src/RendererFactory.cpp`. This is the §12 "renderer exposed through the abstract `Renderer` interface, never through concrete bgfx handles" rule made concrete: the only public-API-reachable code that names `BgfxRenderer` is the one-line factory body.

**Deliberately still private (a recorded next tranche, not a silent gap):** the *richer* surface a full game also touches — `PluginManager`, `World`/`Chunk`/`Voxel`, `ChunkMesh`, `LODManager`, and `platform::Window` — remains under `src/`. These were left private on purpose: they carry deeper dependency graphs (and, in the renderer's case, bgfx-typed methods like `renderChunk`/`setAtlas` on the *concrete* `BgfxRenderer`) that need their own promotion pass before they can be committed without leaking internals. Until that pass lands, an out-of-tree game links the committed core (config + lifecycle + a bgfx-free renderer handle) and reaches the rest through `src/` exactly as the in-tree demos do — those demos remain privileged consumers (they call `BgfxRenderer`'s concrete chunk/atlas methods directly), which is why the abstract `Renderer` interface was widened no further this milestone. Treat `include/` as the **committed** public API and `src/` as internal and subject to change.

---

## 13. Subsystem Dependency Map

```
LayerConfig
    └── validates before all other systems initialize

WorldCoord
    └── used by: World, Layer, Renderer, PhysicsSystem, DecompositionWorker
    └── depends on: nothing (foundational type)

PluginManager
    └── used by: Engine (load/unload)
    └── plugins register into: LayerConfig (recipes), PhysicsSystem (hooks),
                               World (generators), IO (importers/exporters)

World
    └── contains: Layer[]
    └── depends on: LayerConfig, WorldCoord

Layer
    └── contains: chunk cache, voxel grid or MacroVoxel grid
    └── depends on: WorldCoord, LayerConfig, MacroVoxel

MacroVoxel
    └── depends on: recipe registry (via PluginManager), DecompositionWorker

DecompositionWorker
    └── depends on: recipe data, feature generator registry, WorldCoord
    └── must NOT depend on: Renderer, PhysicsSystem, IO

PropagationSystem
    └── depends on: World (read voxel properties + residency)
    └── reports unstable macros to: PhysicsSystem
    └── stops at: immutable layer boundaries and the unloaded-region boundary
                  (conservative: an unknown/non-resident neighbor counts as support)

Window (platform)
    └── depends on: GLFW
    └── produces: NativeWindowHandles (consumed by Renderer)
    └── must NOT depend on: bgfx, Renderer, World, or any engine subsystem

Renderer
    └── depends on: World (read geometry), LODManager, WorldCoord,
                    NativeWindowHandles (from Window, at init only)
    └── must NOT depend on: GLFW, PhysicsSystem, DecompositionWorker, IO

PhysicsSystem
    └── depends on: World (read voxel properties), PropagationSystem,
                    PluginManager (fire on_structural_event)
    └── does NOT write voxels — the structural-response plugin does, via World::setVoxel
    └── must NOT depend on: Renderer, IO, DecompositionWorker

FluidSystem / ThermalSystem (src/simulation/, M14)
    └── depends on: World (read voxel properties: porosity / thermal_conductivity),
                    PluginManager (fire on_fluid_event / on_thermal_event, read registered emitters)
    └── owns its sparse field overlay and writes ONLY to it — never a voxel;
        the mandatory `flow` plugin realizes fluid geometry via the public edit path
    └── must NOT depend on: Renderer, IO, DecompositionWorker

LightingSystem (src/simulation/, M17)
    └── depends on: World (read voxel properties: light_emission, opacity),
                    PluginManager (fire on_lighting_event, read registered light sources)
    └── owns its sparse field overlay (brightness); the mesher reads light levels
        per vertex/face via an optional LightQueryFn callback
    └── must NOT depend on: Renderer, IO, DecompositionWorker

IO (VoxImporter/VoxExporter/QbImporter/QbExporter)
    └── depends on: World (write voxels), LayerConfig (layer assignment)
    └── must NOT depend on: Renderer, PhysicsSystem

NetworkManager (src/net/)
    └── depends on: World (apply incoming edits via setVoxel), PluginManager (fire network hooks),
                    ChunkPersistence/WorldSave (serve dirty chunks on join), ENetTransport
    └── must NOT depend on: Renderer, PhysicsSystem, DecompositionWorker

ENetTransport (src/net/)
    └── depends on: ENet (transport library only)
    └── must NOT depend on: any engine subsystem (it is a pure I/O adapter)

AudioManager (src/audio/)
    └── depends on: PluginManager (read the sound + material-sound registries),
                    WorldCoord, IAudioBackend
    └── listener position pushed in from the front-end (it owns the camera)
    └── must NOT depend on: Renderer, PhysicsSystem, DecompositionWorker, IO, World

MiniaudioBackend (src/audio/)
    └── depends on: miniaudio (audio library only)
    └── must NOT depend on: any engine subsystem (it is a pure output adapter)
```

Keep these dependency boundaries. A subsystem that reaches outside its declared dependencies creates hidden coupling that makes isolated testing and agent-assisted development much harder.

---

## 14. Guidance for AI Coding Agents

This section is written directly for AI coding agents working on this codebase.

### Before You Write Any Code

1. Read this document fully
2. Read `include/plugin_api.h` fully
3. Read the `LayerConfig` validation logic in `src/core/LayerConfig.cpp`
4. Check the subsystem dependency map in Section 13 — if your feature needs a dependency not listed there, that is a design question to raise, not a decision to make unilaterally

### Hard Rules

**Use `WorldCoord` for all world-space positions.** Never use `float`, `double`, `glm::vec3`, or `glm::dvec3` directly for a position in world space. `WorldCoord` is the type. This is enforced by the type system — if you find yourself casting to float for a world-space calculation, you are doing something wrong.

**Do not add non-deterministic calls to the decomposition pipeline.** If you are modifying `DecompositionWorker` or any recipe/feature generator code, you may not use `rand()`, `std::rand()`, `time()`, wall-clock reads, or unordered container iteration without a stable sort. Use the seeded RNG passed into the recipe context.

**Do not shortcut the decomposition chain.** A composite voxel decomposes into its immediate child layer only. It does not skip levels. Do not add logic that attempts to decompose multiple levels in one step.

**Do not cross the subsystem dependency boundaries in Section 13.** In particular: `DecompositionWorker` must not call into the `Renderer` or `PhysicsSystem`. `Renderer` must not call into `PhysicsSystem` or `IO`. These boundaries exist so each subsystem can be understood, tested, and modified in isolation.

**Register plugins via `plugin_api.h`, not by modifying engine internals.** If you are adding a new feature generator, material, or simulation behavior, it belongs in a plugin that registers callbacks. It does not belong as a modification to `PhysicsSystem.cpp` or `World.cpp`.

**Keep engine internals out of public headers.** `include/` is the committed public API; `src/` is private. Do not move a `src/` header into `include/`, and do not expose a private dependency's types (especially `bgfx`) through a public header, without raising it as a design question — the public surface is being decided deliberately. New front-ends are executables that link the `voxel-engine` library; do not add a `main()` to the library or fold a demo back into it. See [Build, Packaging, and the Engine Library Boundary](#12-build-packaging-and-the-engine-library-boundary).

### Common Mistakes to Avoid

- Using `float` arithmetic for a distance or position that could be far from the world origin
- Adding a `static` or global variable to hold cross-subsystem state
- Calling `std::unordered_map::begin()` in deterministic code without sorting first
- Adding a `virtual` method to a core engine class when a registered callback would serve the same purpose
- Modifying the layer config at runtime after `LayerConfig::validate()` has run

### When to Ask vs. When to Proceed

Proceed independently when: adding a new plugin that registers via `plugin_api.h`, adding a new material definition, writing a new feature generator, adding tests.

Raise as a design question when: a new feature requires a dependency not in the Section 13 map, a new feature requires modifying `plugin_api.h`, a new feature requires changing the `WorldCoord` type or the floating-origin pipeline, a new layer mode beyond the three defined ones seems necessary.

---

## 15. Networking and Multiplayer

**Files:** `src/net/NetworkManager.{h,cpp}`, `src/net/ITransport.{h,cpp}`, `src/net/ENetTransport.{h,cpp}`, `src/net/NetPackets.h`, `src/net/NetJoinHandshake.h`, `include/plugin_api.h` (network hook additions). Built-in policy/feature plugins: `plugins/server-authority/`, `plugins/chat/`.

### Design Decisions (M11)

This section records the design that shipped in M11. The subsystem is implemented and exercised by demo 11 (shared world) and the `tests/Network*`/`EditReplication`/`JoinHandshake`/`InterestManagement`/`MessageEnvelope` suite; the decisions below describe the as-built behavior.

### Authority Model

The engine supports two authority models, both implemented as plugin policies rather than hard-coded behavior:

- **Authoritative server** — one dedicated process owns world truth; clients send edit intents, the server validates and broadcasts the result.
- **Host-as-authority P2P** — one peer acts as the authority node; structurally identical to the server model but without a separate process.

**Authority is a policy, not an engine assumption.** The engine asks "who validates this edit?" and defers to whatever is registered. This means a developer can implement a third model (e.g. CRDT-based true P2P with no single authority) as a plugin without modifying the engine core — they implement their own conflict-resolution logic in the `on_edit_received` hook and manage peer consensus themselves. The two supported models are two implementations of the same interface, not special cases baked into the sync path.

### Transport Library

**ENet** (MIT license) is the default built-in transport. It provides reliable and unreliable UDP channels, has no external dependencies, and integrates trivially via CMake subdirectory. It is the right default for the hobbyist and indie audience this engine targets.

The transport sits behind the plugin interface. A developer who needs Steam NAT traversal (GameNetworkingSockets), encryption (yojimbo + libsodium), or any other transport can register a replacement transport plugin. The engine ships with ENet as a zero-configuration starting point and does not trap more capable use cases behind it.

### Sync Strategy

The engine synchronizes only what cannot be re-derived:

- **Shared on join:** `LayerConfig` and the world seed. The joining client re-derives the entire clean world from these two inputs. Only dirty data travels the wire.
- **Ongoing:** player edits (dirty voxel writes) and player positions. Never generated chunks — the deterministic decomposition guarantee (§4) means every client re-derives clean child grids identically from the shared seed and its own approach triggers.
- **On join, dirty chunks:** the server sends the joining client all dirty chunk data for chunks within the client's current interest region. The client overlays these onto its locally re-derived world.

Decomposition events do not need to be synchronized. A remote player triggering decomposition of a macro voxel has no effect on other clients' visual state — each client decomposes on its own approach trigger and gets the identical clean child grid from the seed. If a remote edit lands inside a region a client has not yet decomposed, the edit's `VoxelCoord` is in the child layer; when the client eventually decomposes that macro voxel it loads the dirty chunk (which carries the edit) rather than regenerating it clean — the M5/M7b persistence path handles this transparently.

### Conflict Resolution

**Default: last-write-wins at the authority node.** The authority applies edits in arrival order and broadcasts the committed result. A client whose edit lost receives a correction. This is stateless, has no per-voxel bookkeeping overhead, and is unnoticeable in practice for the single-hosted-session games this engine targets.

**Plugin escape hatch:** the `on_edit_received` hook fires at the authority node *before* an edit is committed. The default implementation returns "apply and broadcast." A developer who needs CRDT, operational transforms, or any other policy registers their own handler. The engine defers to it entirely.

**The authority's edit-application path must be a single choke point.** All edits — regardless of origin — become real through one code path that calls `on_edit_received` before committing. If edits can bypass this path, the escape hatch does not work. This is the same rule as `on_voxel_modified`: one place where voxels change, one hook call there.

### Interest Management

**Default: broadcast all edits to all clients.** A single voxel edit is approximately 64 bytes on the wire (3× int64 coord + material properties + layer id + sequence number). At 10 players all actively editing simultaneously, broadcast traffic per client is under 1 KB/sec — negligible. For the vast majority of indie and hobbyist games, broadcasting eliminates the complexity of interest management entirely.

**Pre-wired option: mirror the local streaming volume.** Each layer's `StreamingVolume` (already computed by `LODManager`) is the network interest boundary: the server only sends a client edits that fall within the chunks that client streams. The filter calls `LODManager::withinViewDistance`, so interest is in lockstep with the **same axis-agnostic box/sphere/shell** the peer streams locally (M16, L6) — it does not re-derive a box and is not left Y-biased after the L1 generalization. No new concept or configuration is required. This is the appropriate choice when player count or world size grows to where broadcast bandwidth is a concern.

**Plugin escape hatch:** an interest-filter plugin can implement arbitrary interest management — per-layer radii tighter than the render distance, faction visibility, line-of-sight, custom spatial queries — by registering a handler the server calls to decide whether a given client should receive a given edit. The engine provides no default geometry for this; the plugin owns the entire decision.

### World-Join Handshake

When a client connects mid-session the server sends, in order:

1. `LayerConfig` + world seed — the client re-derives the entire clean world locally.
2. Dirty chunk data for all chunks within the joining client's current interest region — overlaid on top of the re-derived world.

Nothing else is sent on join. The client does not receive generated chunks (re-derived locally), decomposition state (rebuilt on approach), or out-of-interest dirty chunks (streamed as the client moves).

### Player Messaging

The engine provides a thin typed message envelope usable by any plugin:

```cpp
struct MessageEnvelope {
    const char*  channel_id;   // namespaced by plugin, e.g. "myplugin.trade_offer"
    PlayerId     sender_id;
    MessageTarget target;      // Broadcast | Server | specific PlayerId
    MessageReliability  reliability;  // Reliable | Unreliable
    const void*  payload;      // opaque; plugin owns the schema
    size_t       payload_size;
};
```

The engine routes the envelope according to `target` and `reliability`. ENet's reliable and unreliable channels map directly to `MessageReliability`. The receiving side fires `on_network_message`. Plugins filter by `channel_id`; the engine never inspects the payload.

Clients only ever hold a connection to the authority node, so the authority is also the message router: a `Broadcast` envelope arriving from a peer is relayed to every other connected peer, and a `Player`-targeted envelope is forwarded to its destination peer. On any inbound envelope the authority overwrites `sender_id` with the player id mapped to the connection the packet physically arrived on — the connection is trusted, not the payload, so a peer cannot spoof another player's identity.

**Player chat** ships as a built-in plugin that registers the `"engine.chat"` channel with `Broadcast` + `Reliable` and displays received messages via the HUD debug-text overlay. It is intentionally a plugin — a game that does not want in-engine chat removes it with no engine change.

**Player position updates** use `Unreliable` messages on a `"engine.player_position"` channel. High-frequency, drop-on-lag delivery is correct for position; using the reliable channel would add unnecessary retransmit overhead and head-of-line blocking.

### Plugin API Surface for Networking

M11 added the following to `plugin_api.h`. The existing `on_voxel_modified` hook is extended; all others are new:

| Hook / function | Direction | Purpose |
|---|---|---|
| `on_voxel_modified` (extended) | Engine → Plugin | Post-commit notification, now carries an optional `source` field (local vs. remote `PlayerId`). Single-player plugins that ignore `source` continue working in multiplayer without modification. |
| `on_edit_received` | Engine → Plugin (authority only) | Pre-commit intercept. Called at the authority node before an edit is applied. Returns a `Resolution` (apply / discard / transform). Default built-in: last-write-wins. |
| `on_player_joined` | Engine → Plugin | A player has connected and completed the join handshake. |
| `on_player_left` | Engine → Plugin | A player has disconnected or been dropped. |
| `on_network_message` | Engine → Plugin | A `MessageEnvelope` addressed to this node has arrived. |
| `send_network_message` | Plugin → Engine | Send a `MessageEnvelope`; the engine routes by `target` and `reliability`. |

`on_decomposition_triggered` is intentionally absent. Decomposition state does not require synchronization — see *Sync Strategy* above.

### Dependency Boundaries

`NetworkManager` sits in a new `src/net/` tier. It depends on `World` (to apply incoming edits via `setVoxel`), `PluginManager` (to fire network hooks), and `ChunkPersistence`/`WorldSave` (to serve dirty chunks on join). It must not depend on `Renderer`, `PhysicsSystem`, or `DecompositionWorker`. The transport (`ENetTransport`) is a dependency of `NetworkManager` only — nothing else in the engine knows ENet exists.

---

## 16. Audio

**Files:** `src/audio/AudioManager.{h,cpp}`, `src/audio/IAudioBackend.{h,cpp}`, `src/audio/MiniaudioBackend.{h,cpp}`, `include/plugin_api.h` (audio registration functions, `PluginContext` playback functions, and POD/enum types)

### Design Decisions (M12)

This section records the design that shipped in M12. The subsystem is implemented and exercised by demo 12 (soundscape) and the audio test suite; the decisions below describe the as-built behavior.

### Audio Backend

**miniaudio** (public domain / MIT-0) is the built-in audio backend. It is a single header, fetches via `FetchContent` as easily as bgfx or yaml-cpp, ships a full spatialization engine (listener + positioned sources, distance attenuation, optional Doppler/cones), and decodes WAV/FLAC/MP3 out of the box (OGG via stb_vorbis). Its permissive license fits the static-by-default, per-platform-binary MIT tree without the friction OpenAL Soft's LGPL would add.

miniaudio sits **behind an adapter**, exactly as ENet sits behind `ITransport`: `IAudioBackend` is the abstract seam, and `MiniaudioBackend` is the *only* file that includes `miniaudio.h`. No miniaudio type appears in any public header (`include/`) — the same `PRIVATE`-dependency rule as bgfx and ENet (§12). A developer who wants FMOD, Wwise, or a custom mixer implements `IAudioBackend` and swaps it; nothing else in the engine knows which backend is live.

**Audio is outside the determinism contract (§4).** miniaudio runs its own audio thread off the device callback, so playback never touches the deterministic main-loop path and never feeds decomposition, persistence, or the network wire. The `rand()`/threading rules that bind the decomposition pipeline do not bind audio — sound is presentation, not world state.

### Sound Data Lives Beside the Palette, Not on the Voxel

**A material's sounds live in a `palette_index`-keyed side table — never as a field on `MaterialProperties` or `Voxel`.** This mirrors how visual presentation already works: color is *not* on `MaterialProperties` either; the struct carries only `palette_index`, and the actual color/translucency lives in the 256-entry palette side table keyed by that index (§9, `src/renderer/Palette.h`). Sound is the audio member of exactly that family — presentation keyed by material identity — and belongs in the same kind of side table.

The reasoning is independent of the M8 decision to freeze `MaterialProperties` (§5). Even with a thawed struct, sound would not belong on it:

- **`Voxel` is a POD, replicated across every chunk**, and must stay trivially copyable — it crosses `DecompositionWorker`, the persistence codec, and the network wire, with explicit `_pad[3]` keeping `memcmp`-based determinism checks valid. Sound *data* (paths, decoded buffers) is strings and buffers; it cannot live on a POD regardless. The only thing the struct could hold is an integer handle — and that would re-derive a key the voxel already carries in `palette_index`, growing every voxel, every save, and every packet for nothing.
- **A side table tears down cleanly on plugin unload** (owner-tracked, like the recipe/noise/material registries, §8). Data baked into voxels has the opposite property — §8 already notes that unregistering a material does not retroactively change voxels that already exist. A baked-in sound id would inherit that staleness; a side table resolved at event time does not.
- **It is off the hot path.** The M8 consumption contract (§5) is "read scalar properties by value in simulation; id lookups are tooling-only." Audio triggers on discrete events (break, place, footstep), which is exactly where a lookup is the right tool. The struct's read-by-value advantage exists to serve continuous simulation math (hardness → removal cost); audio does not need it.

Selection is still fully *material-driven*: it is keyed by the voxel's own `palette_index` (one of its material properties), not a hardcoded block-type branch. The registration API names materials by id for authoring ergonomics (`register_material_sound("granite", …)`) but resolves down to a `palette_index` key — the same indirection `register_material` already uses to install a color at an index.

### Positional Model Under the Floating Origin

Sources are stored in `WorldCoord` space and obey the floating-origin rule (§1, §9) exactly as GPU geometry does:

- **The listener is pinned at the local origin; emitters are fed camera-relative floats.** Each tick every emitter is handed to the backend as `source.toLocalFloat(camera)`, with the listener at `(0,0,0)`. World-absolute floats are **never** submitted to the audio engine — the same prohibition as submitting `WorldCoord` directly to the GPU. Moving the listener in world-float instead would reintroduce the precision loss §1 exists to prevent.
- **The listener is set by the front-end, not a plugin hook.** The front-end owns the camera, so it pushes `setListener(WorldCoord pos, forward, up)` into `AudioManager` each frame — the audio analog of how the renderer receives the camera. `AudioManager` is attached to the engine like `NetworkManager` (null when audio is disabled, so existing demos and tests are unaffected) and updated per tick.
- **Units are meters** — `WorldCoord` units — so attenuation distances need no separate audio scale.
- **Two emitter kinds:** fire-and-forget **one-shots** at a `WorldCoord` (break/place/footstep) and persistent **looping emitters** with a lifetime and a (possibly moving) position (ambient beds, flow loops).

### Attenuation: Defaults Are Policy, Not a Ceiling

The engine defaults to **inverse-distance attenuation** with a per-sound **max audible distance** and rolloff factor, and **Doppler off**. These defaults set the acoustic "size" of the world and keep distant edits from accumulating voices.

The backend's other capabilities — linear/exponential rolloff, Doppler factor, min/max distance, cones, per-source volume — stay available as **optional per-sound and per-project overrides**. They are surfaced through the engine's *own* POD types and enums (`AttenuationModel`, `SoundParams`), never by leaking miniaudio types through `include/`. A sound that specifies nothing gets the defaults; one that wants exponential rolloff with Doppler sets it on its `SoundParams`.

### Triggering: Engine Primitives, Behavior as a Removable Plugin

The engine provides audio **primitives** — the sound registries plus the playback/emitter functions — and does **not** auto-play anything. Default behavior ships as a removable plugin, the audio sibling of the M11 chat plugin:

- A built-in **`material-audio` plugin** registers `on_voxel_modified` and calls `play_material_sound` for break/place, resolving each edit's `palette_index` to its registered sound. A game that wants different audio behavior replaces or drops the plugin with no engine change.
- **Footsteps** are fired by the front-end / kinematic path from the ground voxel's material — the engine exposes the lookup-and-play helper; the caller owns the cadence.

Consequently **M12 adds no new event hook to `plugin_api.h`.** Audio rides the *existing* hooks (`on_voxel_modified`, the chunk-lifecycle hooks for decompose, and the structural/flow hooks once M13/M14 land). Because `on_voxel_modified` already carries a `source` field (M11), replicated remote edits produce local sound on every client — but no audio data ever crosses the wire (audio is out of M11's networking scope); each client sounds its own copy of the replicated edit.

### Plugin API Surface for Audio

M12 added the following to `plugin_api.h`. All are new; no existing hook changes.

| Hook / function | Direction | Purpose |
|---|---|---|
| `register_sound` | Plugin → Engine | Name an audio asset by `sound_id` (path + default `SoundParams`). Owner-tracked; torn down on unload. |
| `register_material_sound` | Plugin → Engine | Bind `(material_id, AudioEvent) → sound_id`. Resolves `material_id → palette_index` under the hood (§16 *Sound Data Lives Beside the Palette*). Owner-tracked. |
| `play_sound` | Plugin → Engine (`PluginContext` fn ptr) | Fire a one-shot at a `WorldCoord`, with optional `SoundParams` overrides. |
| `play_material_sound` | Plugin → Engine (`PluginContext` fn ptr) | Resolve `(AudioEvent, palette_index)` to a sound and play it at a `WorldCoord` — the lookup-and-play helper used by `material-audio` and the footstep path. |
| `create_emitter` / `set_emitter_position` / `stop_emitter` | Plugin → Engine (`PluginContext` fn ptr) | Lifecycle for persistent positioned looping emitters; returns an opaque `AudioEmitterId`. |

POD/enum types added alongside: `AudioEvent` (`Footstep`, `Break`, `Place`; `Collapse`/`Flow` reserved for M13/M14), `AttenuationModel`, and `SoundParams`/`EmitterParams` (volume, loop, attenuation model, min/max distance, rolloff, Doppler factor). No `std::` type crosses the plugin ABI, consistent with the M9/M11 surfaces.

The **listener** is intentionally absent from this table — it is set by the front-end via `AudioManager`, not registered by a plugin.

### Missing-Sound Validation

Missing sounds are handled at two distinct points, and conflating them is the mistake to avoid: **runtime resolution is always fail-soft; startup validation is where strictness lives.**

- **At play time, resolution never throws.** A `play_sound`/`play_material_sound` whose `sound_id` or `(AudioEvent, palette_index)` binding does not resolve plays nothing and returns. A missing sound must never crash a running game, and — because audio is a pure sink outside the determinism contract (§4) — must never perturb world state. This holds regardless of build type or config.
- **At startup, a validation pass catches missing sounds up front.** After plugins load — the same stage as recipe validation (§6) — `validateAudio` walks every `register_material_sound` binding (does its `sound_id` resolve?) and every `register_sound` asset (does the file load/decode through the backend?), collecting *all* problems into one report rather than stopping at the first. Whether that report is a hard startup error or a logged warning is a policy, because audio — unlike a malformed `LayerConfig` (§2) — is genuinely optional content.

The policy is tri-state and **defaults to the build type**, so a developer is told immediately without configuring anything:

- `auto` (default) — **error in debug builds** (`#ifndef NDEBUG`), **warn in release**. A game developer hears about a missing sound the moment they run a debug build; a shipped release never hard-crashes on an optional sound that failed to package.
- `error` — always a hard startup error (gate a release in CI on complete audio).
- `warn` — always a warning (run a debug build with knowingly-incomplete audio).

It is set via the project config (`audio.strict: auto | error | warn`, the one-line opt-in pattern `net.interest` established in M11) or passed directly to `validateAudio` for programmatic front-ends. This keeps the engine's "hard errors, not warnings" instinct (§2) where it pays off — the development loop — while honoring that a missing footstep sound, unlike a broken layer stack, is not a reason to refuse to run a player's game.

### Dependency Boundaries

`AudioManager` sits in a new `src/audio/` tier. It depends on `PluginManager` (to read the sound and material-sound registries), `WorldCoord`, and `IAudioBackend`. It must not depend on `Renderer`, `PhysicsSystem`, `DecompositionWorker`, `IO`, or `World` — callers hand it a `palette_index` and a `WorldCoord`, so it never reads world state directly. The backend (`MiniaudioBackend`) is a dependency of `AudioManager` only — nothing else in the engine knows miniaudio exists. The sound and material-sound registries live on `PluginManager` with the recipe/noise/material registries, owner-tracked and torn down on per-plugin unload by the same path.

---

## 17. Fluid and Thermal Simulation

**Files:** `src/simulation/FluidSystem.cpp/.h`, `src/simulation/ThermalSystem.cpp/.h`, `src/core/Tuning.h` (`tuning::fluid` / `tuning::thermal` knobs), `plugins/flow/plugin.cpp` (the mandatory fluid-response plugin). Consumers landed in M14; the design below is the M14 contract. These are the `porosity` and `thermal_conductivity` consumers promised by the §5 property contract.

### Purpose

Fluid spreads through a world gated by each material's `porosity`, and heat diffuses through it at a rate set by each material's `thermal_conductivity`. Like collapse (§7), both respond to the **value of a property on the target voxel**, never to a material identity — a flow or heat system written today works on any material defined later, including modded ones (§5).

### Why This Is Not "M13 Again": Dynamic State

M13's collapse is a pure *read* over binary voxels — instability is derived from material already on the voxel, and the engine writes nothing. Fluid and heat are different in kind: temperature and fluid-amount are **continuous, dynamic quantities that do not exist on the voxel.** `MaterialProperties` holds *constants* (`porosity`, `thermal_conductivity`); the live temperature of a cell and the amount of fluid sitting in it are state that must be advanced every tick. There is no detect-only version of a heat equation — *the diffusion is the simulation.* So M14's first design question is **where that state lives**, and the answer shapes everything else.

### State Lives in Engine-Owned Sparse Overlays

Dynamic field state lives in **engine-owned sparse overlays** keyed by coord — one for temperature, one for in-flight fluid amount — with the **ambient/zero value as the absent-cell default**, so only non-ambient cells are stored. This is chosen over (a) new `Voxel` members, which would force a §9 chunk-format change and cost memory on every voxel for state most voxels never carry, and (b) a dense per-chunk field, which fights the engine's planetary-scale ambitions. The overlays are scoped to the resident region and hold **working state only — never voxel data**, so the §13 "the engine never writes voxels" invariant holds by construction: a field solver writing to its own overlay is not editing the world.

Two **separate** overlays, not one fused cell state: fluid and thermal are **decoupled** in M14 (no boiling, freezing, or lava interplay). Coupling is a deliberate future extension, not an M14 concern.

### Engine Solves the Field; a Plugin Realizes Geometry

The engine owns the **field solver** and writes only to its overlays. When a fluid cell crosses **saturation**, the engine fires `on_fluid_event` (§8) and the **mandatory `flow` response plugin** realizes it as a real fluid voxel through the public edit path (`apply_edit` → `World::setVoxel`), exactly the detect/respond feedback loop M13 established for collapse. This is the deliberate consequence: **fluid that has settled is an ordinary voxel** — it collides, it can be edited, and it persists through the normal §9 chunk path for free. With no `flow` plugin loaded, fluid still *simulates* as a field but never becomes geometry — the legitimate "no fluid voxels" configuration, the fluid analog of "no structural plugin ⇒ no cave-ins." Thermal events (`on_thermal_event`) are the same detect-and-report shape; what a plugin does with a temperature crossing (ignite, melt, play the reserved `AudioEvent::Flow`/audio) is game policy.

### The Solver: Explicit Cellular-Automaton Relaxation

Both systems are **explicit, deterministic, neighbor-relaxation passes** over the sparse active set plus its 6-connected frontier — the same neighbor-walk shape as the M13 support flood, at **terminal-voxel granularity bounded to the resident region**. There is **no LOD aggregation** up the composite chain (unlike M13's macro aggregate): a per-coord field has nothing to aggregate, and coarse-layer diffusion is neither meaningful nor affordable.

- **Heat** — explicit finite-difference diffusion (`dT/dt = α∇²T`), the coefficient `α` derived from each cell's `thermal_conductivity` (conductivity-only; a flat heat capacity for M14, with density-derived capacity left as a later refinement). The explicit scheme is only conditionally stable, so the pass **sub-steps to respect the stability bound** (3D: `α·dt/dx² ≤ 1/6`) rather than taking one large step.
- **Fluid** — cellular-automaton level/head flow **gated by `porosity`**: `0` blocks flow entirely (the demo's low-`porosity` wall), `1` is fully permeable, and **air/empty is treated as effective porosity 1.0** so fluid flows freely through open space and is stopped only by solid low-`porosity` material.

An implicit/global solve (matrix or pressure projection) was rejected: it wants a dense field, fights the sparse overlay, and is far harder to make deterministic and to budget.

### Sources: Plugin-Registered Emitters

Heat and fluid originate from **plugin-registered emitters** — `register_heat_source` / `register_fluid_source` (§8) — that the engine injects into the overlays each tick. They are owner-tracked in `PluginManager` and torn down on unload like the recipe/material/sound registries. There is **no material-baked source**: a "lava radiates heat" effect is a plugin registering an emitter at the lava's location, not an automatic property of the material. This keeps source *placement* a game decision while the *diffusion* stays an engine primitive.

### Determinism, Budget, and Persistence

Both passes obey the §4 determinism contract: they run **end-of-frame**, visit cells in **deterministic sorted-coord order** (no unordered iteration, no `rand`/`time`), and are bounded by `tuning::fluid` / `tuning::thermal` budgets with **overflow carried to the next frame**, so a large flood or heat front spreads across frames instead of stalling one — the `tuning::physics` pattern from M13.

The overlays are **transient**. Durable fluid is the realized *voxels* the `flow` plugin already placed (persisted by §9 with no format change); on load, both fields are re-derived from re-registered emitters and the existing fluid voxels. Heat resets to ambient on load and re-warms from its emitters — acceptable, and the same "state re-establishes from its source" stance the transient `RemovalAccumulator` took (§5). Network replication of the field itself is deferred: the realized fluid voxels already replicate through the M11 edit choke point, so remote clients see the same geometry without the field crossing the wire.

---

### Lighting Overlay (M17, A1)

**Files:** `src/simulation/LightingSystem.cpp/.h`, `src/core/Tuning.h` (`tuning::lighting` knobs).

A third engine-owned sparse field overlay, following the same architecture as thermal and fluid: a `FieldOverlay` of per-voxel brightness, advanced once per frame after fluid, scoped to the resident terminal-layer region.

**Two light channels, one overlay value:**

- **Sky light** — a voxel with no opaque block above it in the resident region receives full brightness (`kMaxBrightness = 1.0`). Sky access is a per-column check, not a lateral propagation.
- **Block (emitter) light** — materials with `light_emission > 0` (a new `MaterialProperties` field) and plugin-registered point sources (`register_light_source`, §8) inject brightness that propagates via BFS through transparent voxels, attenuating by `kAttenuationPerStep` per hop. The effective range of a full-power emitter is `1.0 / kAttenuationPerStep` = 15 voxels.

Both channels are combined into a single overlay value per cell (max wins). The mesher (`buildChunkMeshData`) accepts an optional `LightQueryFn` callback and multiplies each face's vertex color by the sampled light level at the neighboring air voxel; when no callback is provided, the pre-M17 fixed directional shading is byte-identical.

**Rebuild, not incremental:** Each tick the overlay is cleared and rebuilt from scratch (sky columns + emitter BFS), bounded by `kMaxLightingCellsPerFrame`. This is simpler and more correct for light removal (a block placed above a column immediately darkens everything below it) at the cost of per-tick work proportional to the lit region size. The dirty flag (`on_voxel_modified` hook) skips the rebuild when nothing changed.

**Events:** `on_lighting_event` fires on active-set boundary crossings (Rising/Falling), mirroring `on_thermal_event`. Same detect/respond split: what a plugin does with a lighting change (e.g., trigger mob spawning in darkness) is game policy.

**Persistence:** The overlay is transient — re-derived each tick from sky geometry, material emitters, and registered sources. No §9 format change beyond the `light_emission` field on `MaterialProperties` (`.vxc` version bumped to 2).

---

### Ambient Occlusion (M17, A2)

**Files:** `src/renderer/ChunkMeshData.cpp` (the mesher), `src/core/Tuning.h` (`tuning::ao` knobs).

Per-vertex ambient occlusion ("smooth lighting") darkens concave voxel corners. Unlike the lighting overlay above, AO needs **no field, no overlay, no persistence** — it is a purely mesher-local computation folded into the vertex color the same way the directional shade and the optional light level are.

For each face vertex, the mesher reads the 2×2 block of voxels around that corner in the air layer one step outside the face (voxel + face normal): two edge-adjacent cells and the diagonal cell. The standard kernel maps these to an occlusion level — `0` when two opposing sides enclose the corner, otherwise `3 − (sides + corner)` — and `tuning::ao::kVertexFactor[level]` is the brightness multiplier (level 3 = open corner = `1.0`, so flat/convex terrain is unchanged). The four corner levels also drive the **quad-flip**: the face splits along whichever diagonal isolates the darker corner, avoiding the asymmetric shading smear that fixed triangulation produces at a single dark corner. Both triangulations stay CCW-outward, so face culling is unaffected.

AO is deliberately scoped to the chunk being meshed: out-of-chunk and translucent neighbors count as non-occluding, so a concave corner that straddles a chunk seam is faintly under-darkened there — an accepted trade for keeping the mesher free of cross-chunk neighbor plumbing.

---

## 18. Gravity Provider and Axis-Agnostic Kinematics

**Files:** `src/world/GravityProvider.h`, `src/world/AxisRole.h`, `src/world/VoxelCollision.{h,cpp}`, `src/simulation/FluidSystem.{h,cpp}`, `src/renderer/MaterialFaces.{h,cpp}`, `src/renderer/ChunkMeshData.{h,cpp}`, `src/world/ResolvedRecipe.{h,cpp}`

### Gravity Is a Policy, Not a Baked Force (M16, L7)

Several Phase-1 implementation choices quietly assumed a single privileged "down" axis (−Y): collision grounding hard-coded `axis == 1`, the fluid solver drained into `{x, y−1, z}`, and the M15 textured-face / recipe-boundary code painted the "top" skin on +Y. That narrows the engine toward a Y-up block game even though the README promises flying, planetary, and space configurations. M16 turns the implicit assumption into an explicit, configurable **policy**.

The seam is `GravityProvider::gravityAt(WorldCoord) → dvec3` — a per-position "down" vector, exactly the way authority and interest are policies (§15) rather than engine assumptions. It is a small, copyable value type with three built-in shapes plus a `custom` function-pointer escape hatch:

- **`constant(dir)`** — a fixed axis. The engine default is `constant(-Y)`, so **every existing demo and test is byte-for-byte unchanged**.
- **`radial(center, strength)`** — "down" is the unit vector toward a body's center; the basis for a per-asteroid gravity well (a player walks around and mines a body from any side).
- **`zeroG()`** — the zero vector; no privileged direction anywhere.

Gravity may vary **per position** (radial) and **per frame** (a moving body), so consumers query it each step rather than caching a global axis. Nothing on `MaterialProperties` / `Voxel` changes — gravity is never stored on world data.

### The Consumers — Reading the Same "Down"

**Collision grounding (L2, `VoxelCollision`).** The swept-AABB resolution is already per-axis symmetric; only the *interpretation* of `grounded` read the axis. It is now derived from "blocked **along** the gravity vector": when a sub-step is blocked on an axis, `grounded` is set iff the movement direction has a positive component along the supplied `gravity_dir` (`sign(d[axis]) * gravity_dir[axis] > 0`). Under the default −Y this is exactly "blocked while moving down"; an alternate fixed axis lets a player stand on a wall; a per-position radial vector lets one walk the +X face of an asteroid; zero gravity makes the product zero, so there is **no grounded concept**. The per-axis `hitX/hitY/hitZ` blocking is untouched — only `grounded` reads gravity.

**Fluid flow (L3, `FluidSystem`).** The Phase-A drain / Phase-B lateral-equalize split is parameterized by the per-cell gravity vector instead of a fixed −Y. A neighbor is a **drain** target when its direction has a positive component along gravity (`dot > 0`); the **lateral** equalization runs across neighbors perpendicular to gravity (`dot == 0`). Under constant −Y exactly one drain neighbor (−Y) and four lateral ones (±X/±Z) qualify — identical to M14. A radial well drains toward the center from several sides at once; under zero-g no neighbor is downhill, so the pass degenerates to pure 6-neighbor pressure equalization. (A downhill cascade can hand fluid forward several hops in one pass, so the commit set is every cell that received a delta, not just the frontier work set — under −Y that set equals the work set, preserving the byte-identical M14 result.)

**Face roles (G1/G2, `axisrole::roleOf`).** The appearance and decomposition tiers author faces by **role** — `up` (top skin), `down` (bottom), `lateral` (side) — and resolve the role of each geometric face against gravity at query time. The mesh builder (`materialfaces::faceTile`) shows a material's `top` tile on whichever geometric face most opposes gravity, so grass renders side-out on an asteroid's +X face; the recipe boundary distribution (`RecipeDesc::BoundaryDesc` via `fillChildChunk`) lands the `top`/`bottom`/`side` distributions on the gravity-relative macro faces, so a decomposed crust is radial rather than a flat +Y slab. Both default to constant −Y (up = +Y), reproducing the M15 Y-up mapping byte-for-byte.

**Shade ramp (G3, `ChunkMeshData`).** The mesher's fixed fake-lighting ramp resolves each face's role through the same `axisrole::roleOf` so the up-facing face is brightest and the down-facing darkest *relative to gravity*, not always +Y — the lit relief now agrees with the gravity-defined surface instead of contradicting it on an off-axis body. Default −Y is the historical ramp byte-for-byte.

**Camera up (M17, `Renderer::setCameraUp`).** The view basis and frustum align the camera's up-axis to the supplied up (typically `-gravityDir`), so the rendered horizon is level on whatever surface the player stands on — the appearance counterpart to L2's grounding. Default +Y is the historical view byte-for-byte (see §9, *Camera Orientation*). Unlike the other consumers this one is supplied to the renderer once per frame rather than per cell/face, but it reads the same "down".

### Dependency Boundaries

`GravityProvider` and `axisrole` are header-only world-tier facilities with no dependencies beyond `glm` / `WorldCoord`, so both the world tier (collision) and the simulation tier (fluid) and the renderer (face roles) read them without a new edge. The gravity vector is **supplied to** the kinematic step / passed to the mesh builder by the host or demo, which owns the `GravityProvider` and queries it — the engine never installs a global gravity singleton, matching the policy stance of §15.

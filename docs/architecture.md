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
7. [Upward Damage Propagation](#7-upward-damage-propagation)
8. [Plugin System](#8-plugin-system)
9. [Renderer and LOD](#9-renderer-and-lod)
10. [Import/Export and Editor Interoperability](#10-importexport-and-editor-interoperability)
11. [Persistence and Dirty Tracking](#11-persistence-and-dirty-tracking)
12. [Build, Packaging, and the Engine Library Boundary](#12-build-packaging-and-the-engine-library-boundary)
13. [Subsystem Dependency Map](#13-subsystem-dependency-map)
14. [Guidance for AI Coding Agents](#14-guidance-for-ai-coding-agents)

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

**Files:** `src/core/LayerConfig.cpp/.h`

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
| Single-decomposition child grid count exceeds `max_decomposition_voxels` config | Warning, child count printed |

### Why Hard Errors, Not Warnings

A composite layer with no recipe, or a non-integer ratio, has no valid runtime fallback. Allowing the engine to start and then failing when a player first interacts with the broken layer produces a much harder-to-diagnose problem. Startup validation fails fast with actionable messages.

### Child Grid Size Warning

When a 1000:1 ratio is configured, one composite voxel decomposing generates 10⁹ child voxels. This will exhaust memory on any current hardware. The warning prints the actual number so the game designer understands the consequence before hitting it in a playtest.

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

---

## 5. Material Property System

**Files:** `src/world/Voxel.h`, `src/simulation/PhysicsSystem.h`

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
| `hardness` | `float` | Relative mining resistance; not mapped to any real-world scale |
| `palette_index` | `uint8_t` | Index into the 256-entry visual palette; used for `.vox` compatibility |

### Palette Index and Editor Compatibility

`palette_index` is the bridge to standard voxel editors. The visual palette maps each index to color, texture, and PBR parameters. A `.vox` file stores only palette indices — importing a `.vox` file populates `palette_index` and leaves other material properties at their defaults for that palette entry. Extended properties are stored in the engine-native `.vxe` format.

---

## 6. Composition Recipes and Feature Generators

**Files:** `src/world/MacroVoxel.h`, `include/plugin_api.h`

### Recipe Structure

A composition recipe is a data record attached to a composite voxel type. It contains:

- **Material distribution spec** — a list of `(material_id, weight)` pairs plus a noise function ID and parameters controlling spatial arrangement
- **Feature generator list** — ordered list of `(feature_generator_id, parameters)` entries applied after material distribution
- **Boundary overrides** — separate material distribution specs for the top, bottom, and side faces of the macro-voxel
- **Seed parameters** — arbitrary key-value pairs passed to child recipes as generation constraints

### Feature Generators

Feature generators are plugins that receive a partially-filled child grid and a parameter set, and stamp spatial structures into it. They are applied in declaration order. Examples:

- Cave network generator: carves void regions using 3D noise
- Ore vein generator: replaces material pockets with ore materials
- Water table generator: fills below a threshold depth with fluid material
- Dungeon seed generator: places pre-authored static structures at low probability

Feature generators are identified by string name. Recipes reference them by name. If a recipe references a feature generator whose plugin is not loaded, it is a startup error (not a silent skip).

### Hierarchical Constraints

A recipe can include `seed_parameters` that are passed to child composite voxels' recipes as generation inputs. This allows a 10km "mountain range" recipe to constrain its constituent 1km "peak" recipes — e.g. "generate peaks biased toward granite, with no water table above 800m" — without either recipe needing to know about the other's implementation.

---

## 7. Upward Damage Propagation

**Files:** `src/simulation/PropagationSystem.cpp/.h`

### Purpose

When a player mines child voxels out of a composite voxel, the composite voxel's effective `density` and `structural_strength` decrease. If enough material is removed, the composite voxel itself can become structurally unsound at its own scale and collapse — potentially cascading to neighbors.

This emergent behavior requires no special-case logic beyond: after any child voxel modification, recompute the parent composite voxel's aggregate properties and check structural integrity.

### Propagation Rules

- Aggregate properties are computed as volume-weighted averages of child voxel material properties
- If `structural_strength` falls below the threshold for the load the voxel is bearing, a collapse event fires
- Collapse events are processed by the `PhysicsSystem`, which may trigger further modifications to neighboring voxels
- Propagation walks upward through composite layers only
- **Propagation stops at an immutable layer boundary** — immutable voxels have fixed properties and cannot be structurally compromised

### Performance Note

Recomputing aggregate properties on every child modification is expensive at fine granularity. The system uses a dirty-aggregate pattern: child modification marks the parent aggregate as stale, and recomputation is deferred to end-of-frame. Do not trigger aggregate recomputation inline in the voxel modification path.

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

// Material definitions
void register_material(const char* material_id, MaterialProperties props);

// Import/Export
void register_importer(const char* extension, ImporterFn fn);
void register_exporter(const char* extension, ExporterFn fn);

// Simulation
void register_on_voxel_modified(OnVoxelModifiedFn fn);
void register_on_structural_event(OnStructuralEventFn fn);

// Layer lifecycle
void register_on_chunk_created(const char* layer_name, ChunkLifecycleFn fn);
void register_on_chunk_evicted(const char* layer_name, ChunkLifecycleFn fn);
```

See `include/plugin_api.h` for full signatures and `src/plugins/ExamplePlugin` for a worked example.

### Plugin Load Order

Plugins are loaded in declaration order from the project config. Hook registrations from earlier plugins are visible to later plugins. If two plugins register the same feature generator ID, the second registration overwrites the first with a logged warning.

---

## 9. Renderer and LOD

**Files:** `src/renderer/Renderer.cpp/.h`, `src/renderer/BgfxRenderer.cpp/.h`, `src/renderer/LODManager.cpp/.h`, `src/platform/Window.cpp/.h`, `src/platform/NativeWindowHandles.h`

### Windowing and Surface

The window is owned by a separate platform layer (`src/platform/Window`, backed by GLFW), not by the renderer. The window creates no graphics context of its own (`GLFW_NO_API`) — bgfx owns the device and renders into the window's native handle.

The seam between the two is `platform::NativeWindowHandles`, a small struct carrying the native window/display pointers (and a Wayland flag). The window layer produces it; the renderer consumes it to populate `bgfx::PlatformData` in `Renderer::initialize`. This keeps the dependency one-directional and library-neutral: **the window layer never includes bgfx, and the renderer never includes GLFW.** Do not collapse these — a renderer that pulls in GLFW, or a window that pulls in bgfx, defeats the separation that lets either backend be swapped.

bgfx is initialized in single-threaded mode (a `bgfx::renderFrame()` call precedes `bgfx::init`), so device and window calls stay on the main thread — required on macOS and simpler everywhere.

**Linux:** X11 is requested explicitly for now (`GLFW_PLATFORM_X11`); native-handle wiring for Wayland is a planned follow-up. On a Wayland session the window runs through XWayland until then.

### Layer-Aware Rendering

Each layer has its own view distance and chunk budget, configured per-layer. A 100km layer needs very few visible chunks at any one time (the player can see maybe 3–4 in any direction). A 1m terminal layer needs many small chunks in a tight radius. Sharing a single view-distance setting across all layers would either waste memory on macro-layers or starve the terminal layer.

`LODManager` maintains per-layer chunk visibility sets and evicts chunks that fall outside the view distance budget.

### Floating Origin

The renderer maintains the camera's current position as a `WorldCoord`. Before any geometry is submitted to the GPU, vertex positions are converted to camera-local space via `WorldCoord::toLocalFloat(camera_position)`. The result is a `glm::vec3` of small magnitude that fits safely in 32-bit float precision.

**Do not submit `WorldCoord` values directly to the GPU.** Always go through `toLocalFloat`.

### Immutable Layer Rendering

Immutable layer voxels are rendered as static geometry with no chunk management overhead. They do not participate in the dirty/evict cycle. Their mesh is generated once at world load and retained.

### Renderer Backend Targets

bgfx selects its graphics backend at compile time. The targeted backends per platform are:

- **Windows** — Vulkan (preferred), Direct3D 12 as fallback
- **Linux** — Vulkan
- **macOS** — Metal

Do not write renderer code that assumes a specific underlying API. All GPU interaction must go through bgfx's abstraction layer. In particular: do not use Vulkan-specific extensions, Direct3D-specific resource hints, or Metal-specific texture formats directly — if something requires platform-specific behavior, it belongs behind a bgfx capability query (`bgfx::getCaps()`), not a compile-time `#ifdef`.

The compile-time backend selection also means the project ships separate binaries per platform rather than a single binary that selects at runtime. This is a known bgfx tradeoff and an acceptable one for this project's scope.

---

## 10. Import/Export and Editor Interoperability

**Files:** `src/io/VoxImporter.cpp/.h`, `src/io/VoxExporter.cpp/.h`

### Interoperability Philosophy

Standard voxel editors (MagicaVoxel, Qubicle, Goxel) work with single-scale, palette-indexed cubic grids. The engine supports them for the common case — single-layer, palette-material content — and progressively diverges only when a game maker opts into extended features.

### `.vox` Import

A `.vox` file is always imported into a specific layer at a specific world-space anchor. The importer populates `palette_index` for each voxel. Other material properties are initialized from the palette entry's default property set. The engine does not attempt to infer material properties from color values.

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

Dirty tracking is at **chunk granularity within a composite voxel**, not per-voxel. A chunk is a fixed-size subvolume of a layer (size is a tunable constant, default TBD). When any voxel in a chunk is modified, the chunk is marked dirty and scheduled for persistence.

Per-voxel dirty tracking was rejected because a single mining session could mark millions of individual voxels dirty, producing save file write amplification that makes autosave impractical.

### What Gets Persisted

- All dirty chunks (player-modified)
- Composite voxel decomposition state (whether a given composite has been decomposed, so we don't re-decompose on load)
- Immutable voxels: nothing (regenerated from seed on load)
- Clean (unmodified) recipe-generated chunks: nothing (regenerated on cache miss)

### Cache Eviction

Clean chunks can be evicted from memory when they fall outside the LOD view distance budget. On cache miss (player re-approaches), they are regenerated deterministically from the recipe. This is transparent to the player only if decomposition is deterministic — see [Cascading Decomposition](#4-cascading-decomposition).

Dirty chunks are never evicted from disk, but may be evicted from the in-memory cache and reloaded on demand.

---

## 12. Build, Packaging, and the Engine Library Boundary

**Files:** `CMakeLists.txt`, `include/`, `src/`, `demos/`, `tests/`

### The Engine Is a Library; Front-Ends Are Separate Executables

The engine builds as a library target (`voxel-engine`) compiled from everything under `src/`. It contains no `main()`. Games, demos, development tools, and the test suite are **separate executable targets that link the library**:

```
voxel-engine        (library)     ← all of src/**
demos/sandbox       (executable)  ← development launcher; links voxel-engine
demos/<milestone>   (executable)  ← per-milestone demos; each links voxel-engine
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

- **`include/` is `PUBLIC`** — the committed public API. It propagates to every consumer that links the library. Currently `WorldCoord.h` and `plugin_api.h`.
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

### Not Yet Finalized

The exact public-header surface is a deliberate open decision, not the current state frozen. Consumer-facing types such as `Engine`, `LayerConfig`, and `PluginManager` still live under `src/` and are reached by in-tree demos directly. Promoting the genuinely public ones into `include/` — and exposing the renderer behind a creation factory so bgfx stays entirely out of the public API — is a planned follow-up. Until then: treat `include/` as the **committed** public API and `src/` as internal and subject to change.

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
    └── depends on: World (read voxel properties), PhysicsSystem (structural events)
    └── stops at: immutable layer boundaries

Window (platform)
    └── depends on: GLFW
    └── produces: NativeWindowHandles (consumed by Renderer)
    └── must NOT depend on: bgfx, Renderer, World, or any engine subsystem

Renderer
    └── depends on: World (read geometry), LODManager, WorldCoord,
                    NativeWindowHandles (from Window, at init only)
    └── must NOT depend on: GLFW, PhysicsSystem, DecompositionWorker, IO

PhysicsSystem
    └── depends on: World (read/write voxel properties), PropagationSystem
    └── must NOT depend on: Renderer, IO

IO (VoxImporter/VoxExporter)
    └── depends on: World (write voxels), LayerConfig (layer assignment)
    └── must NOT depend on: Renderer, PhysicsSystem
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

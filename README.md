# Voxel Game Engine

## Overview

The Voxel Game Engine is a plugin-based C++ game engine designed for creating voxel-based games that go well beyond the conventional single-scale Minecraft model. Its defining architectural features are:

- **Hierarchical multi-layer scale system** — game makers define any number of voxel layers, each with its own base unit size, from centimeters to kilometers
- **Three voxel modes** — Composite (lazily decomposes on demand), Immutable (collision/rendering only, no decomposition), and Terminal (player-buildable leaf layer)
- **Cascading lazy decomposition** — macro-voxels decompose one layer at a time through a chain of intermediate composite layers, never jumping scales in a single step
- **Material-driven simulation** — voxels carry physical properties rather than hardcoded block type logic, enabling composable modding and emergent simulation
- **Standard tool interoperability** — compatible with `.vox` (MagicaVoxel) and `.qb` (Qubicle) for single-layer content; extended engine-native format for multi-layer and material features
- **Plugin architecture** — nearly all engine behavior including world generation, features, physics, and import/export is extensible via plugins
- **AI agent-friendly design** — explicit invariants, a strong `WorldCoord` type, flat callback-based plugin hooks, and dedicated architecture documentation

The engine is designed to support games ranging from a conventional Minecraft-style single-scale world all the way to a multi-scale planetary simulation where players build meter-scale structures inside hundred-kilometer terrain voxels — or a flying game where the terrain is pure backdrop and only a small playspace layer is interactive at all.

---

## AI Coding Agent Friendliness

This engine is deliberately designed to work well with AI coding agents (Copilot, Claude Code, Cursor, and similar tools). This is not an afterthought — several internal design decisions exist specifically to make agent-assisted development reliable rather than subtly wrong.

Concretely, this means:

- **Invariants are machine-enforced, not just documented.** The `WorldCoord` type makes accidental float promotion a compile error. The `LayerConfig` validator makes invalid layer configurations a startup error. Constraints that only exist in comments get silently violated; constraints enforced by the compiler or runtime do not.
- **The plugin API is flat callback registration, not deep inheritance.** An agent can read `include/plugin_api.h` and understand the complete set of extension points without tracing a class hierarchy. Each plugin's contract is self-contained.
- **Subsystem dependencies are explicit and bounded.** The architecture document defines which subsystems may depend on which others. An agent working on decomposition does not need to understand the renderer. An agent working on the renderer does not need to understand physics.
- **Negative rules are written down explicitly.** The things you must *not* do — introduce float arithmetic in world-space paths, add non-deterministic calls to the decomposition pipeline, skip levels in the decomposition chain — are documented as named rules, not left to be inferred from context.

**If you are an AI coding agent starting work on this codebase, read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) before writing any code.** It contains the subsystem map, the hard invariants, a list of common mistakes, and a heuristic for when to proceed independently versus when to raise a design question. Working without it will produce code that looks correct but violates load-bearing constraints in ways that are difficult to diagnose.

---

## Core Architectural Concepts

### Multi-Layer Scale System

Rather than fixing a single voxel size, the engine supports a user-defined stack of **layers**, each representing a different scale of the world. A game defines its layer stack in a project configuration file:

```yaml
layers:
  - name: "continental"
    voxel_size_m: 100000.0
    mode: composite
    decompose_to: "regional"

  - name: "regional"
    voxel_size_m: 10000.0
    mode: composite
    decompose_to: "local"

  - name: "local"
    voxel_size_m: 1000.0
    mode: composite
    decompose_to: "terrain"

  - name: "terrain"
    voxel_size_m: 1.0
    mode: terminal

  - name: "detail"
    voxel_size_m: 0.1
    mode: terminal
```

A minimal flying game that uses large voxels only as backdrop:

```yaml
layers:
  - name: "world"
    voxel_size_m: 100000.0
    mode: immutable       # collision + rendering only; no decomposition ever

  - name: "playspace"
    voxel_size_m: 1.0
    mode: terminal        # the only interactive layer
```

A conventional Minecraft-style game defines a single terminal layer and never interacts with any of this complexity.

**Validation rules enforced at engine startup:**

- At least one layer must be defined
- Each layer's voxel size must be strictly smaller than its parent
- The ratio of parent size to child size must be a whole integer (e.g. 10:1 or 100:1 — not 7.3:1)
- Minimum ratio between adjacent layers is 2:1
- Every composite layer must either name a valid `decompose_to` target or have a recipe plugin registered — a composite layer with no recipe is a startup error, not a runtime surprise
- Ratios implying a single-decomposition child grid exceeding the configured voxel budget produce a warning with the actual child count printed explicitly

**Why integer ratios only?** A parent voxel's interior must tile perfectly with child voxels. A 10m parent with 1m children produces exactly 10×10×10 = 1,000 child slots. A 7.3m parent produces a fractional remainder that cannot be represented cleanly in a grid. Non-integer ratios are rejected at load time with a clear error message, not silently rounded.

### Voxel Modes

Every layer operates in one of three modes, declared in the layer config:

| Mode | Decomposition | Player Modification | Persistence | Use Case |
|---|---|---|---|---|
| `composite` | Lazy, on demand | After decomposition | Dirty chunks only | Procedural terrain at any intermediate scale |
| `immutable` | Never | Never | None needed | Backdrop geometry, skybox-as-collision, "floor is lava" terrain |
| `terminal` | N/A (leaf layer) | Yes | Always | Player-buildable space; the finest interactive scale |

Immutable voxels do not participate in upward damage propagation. The propagation chain stops at an immutable boundary.

### Cascading Decomposition

Decomposition is a **chain**, not a single jump. A 100km composite voxel does not decompose directly into 1m child voxels. It decomposes into its declared `decompose_to` layer (e.g. 10km composites), which in turn decompose into their children, and so on, until a terminal layer is reached.

Each step in the chain is independently lazy — intermediate composite layers materialize only when something interacts with them. A player approaching a 100km voxel triggers decomposition to 10km children in the immediate area. Drilling further down triggers decomposition of the relevant 10km children into 1km children, and so on.

Recipes at each composite level can pass **seed parameters and material biases** down to their children, enabling hierarchical constraint: a "mountain range" 10km recipe can constrain what its constituent 1km "peak" recipes generate, without either layer knowing how many levels exist above or below it.

### Coordinate Precision

World-space coordinates are stored and computed using the `WorldCoord` type, which wraps a double-precision 3D vector. Only the final GPU submission path converts to single-precision float, using a **floating origin** — the camera's world position becomes the local origin, and all scene geometry is translated into camera-local space before submission.

This prevents the floating-point precision loss that silently corrupts sub-meter detail at kilometer scales (a 32-bit float has ~7 significant decimal digits; at 100km scale, sub-meter precision is gone entirely).

`WorldCoord` provides explicit named conversion methods rather than implicit casts. This makes accidental float promotion a compile error rather than a silent bug. **Do not use raw `double` or `float` for world-space positions anywhere in the engine or in plugins.**

### Material-Driven Simulation

Voxels do not have a hardcoded "block type ID." Instead, each voxel carries a set of **material properties**:

| Property | Description |
|---|---|
| `density` | Mass per unit volume; affects physics and structural load |
| `structural_strength` | Resistance to collapse under load |
| `thermal_conductivity` | Heat transfer rate; relevant for fire/temperature simulation |
| `porosity` | Fluid permeability |
| `hardness` | Resistance to mining/destruction |
| `palette_index` | Maps to a visual material definition (color, texture, PBR params) |

Mods that add new materials define property values rather than special-case code. Physics, fluid, and mining systems respond to properties, not IDs. A new volcanic rock mod does not require changes to lava flow logic — it just declares material properties that the existing fluid system already understands.

For voxel-editor compatibility, the `palette_index` field maps to a standard 256-entry palette, allowing `.vox` import/export for the visual material layer even when extended properties are in use.

### Macro-Voxel Composition Recipes

Any composite voxel carries a **composition recipe** describing what it contains at the next layer down. Recipes specify:

- **Material distribution** — a weighted palette of child materials with a noise function controlling spatial arrangement (e.g. "80% granite, 15% quartz veins, 5% iron ore")
- **Feature overlays** — spatially-aware generators that stamp structures into the child grid (cave networks, water tables, ore clusters, dungeon seeds, etc.)
- **Boundary behavior** — how the top, bottom, and sides of the macro-voxel differ from its interior (surface soil, exposed rock faces, ice caps, etc.)
- **Seed parameters** — values passed down to child recipes to constrain their generation

Feature generators are registered by plugins. A recipe references feature generators by name. Cave generation, dungeon placement, and any other sub-structure system are plugin-defined and fully composable.

### Lazy Decomposition Details

When a player interacts with an undecomposed composite voxel:

1. The engine checks whether a decomposed child grid already exists for that voxel
2. If not, the recipe runs on a worker thread to generate the child grid; async pop-in avoids hitching the main thread
3. The interaction proceeds against the now-real child voxels (which may themselves be composites requiring further decomposition)
4. Player-modified child chunks are marked **dirty** and persisted; unmodified recipe-generated chunks can be evicted and regenerated on demand

**Dirty tracking is at chunk granularity within a composite voxel**, not per-voxel, to keep save file sizes tractable. Chunk size is a tunable constant.

**Upward damage propagation:** Material loss in child voxels updates the parent composite voxel's aggregate `density` and `structural_strength`. A heavily hollowed composite voxel can become structurally unsound at its own scale, triggering collapse that may cascade to neighbors. This emergent behavior arises from the material property system without special-case logic. Propagation stops at immutable layer boundaries.

Decomposition is **deterministic** — given the same recipe and world seed, the same child grid is always generated. This is what allows unmodified chunks to be evicted and regenerated transparently.

### Voxel Editor Interoperability

The engine is designed to remain compatible with standard voxel editors for the common case:

| Content type | Format | Compatibility |
|---|---|---|
| Single-layer, palette materials | `.vox`, `.qb` | Full import/export |
| Multi-layer or anchored content | Engine-native `.vxe` + `.vox` sidecar | Import/export via plugin |
| Extended material properties | `.vxe` | Engine-native only |

A plugin that adds non-standard features should also register an import/export handler. If none is registered, the engine falls back to vanilla `.vox` export (lossy but functional, with a logged warning).

The `.vox` format supports volumes up to 256³ per object. Larger volumes are automatically chunked on import/export. Imported `.vox` content is always assigned to a specific layer and world-space anchor; it has no concept of the other layers.

---

## Project Structure

```
voxel-game-engine
├── src                                   # → voxel-engine library (all sources below)
│   ├── core
│   │   ├── Engine.cpp / .h               # Engine lifecycle, startup validation
│   │   ├── PluginManager.cpp / .h        # Plugin load/unload, hook registration
│   │   └── LayerConfig.cpp / .h          # Layer stack definition and validation
│   ├── world
│   │   ├── Voxel.cpp / .h                # Voxel data: material props, palette index, mode
│   │   ├── Layer.cpp / .h                # Per-layer chunk management and coordinate space
│   │   ├── World.cpp / .h                # Multi-layer world container
│   │   ├── MacroVoxel.cpp / .h           # Composition recipe, decomposition state, mode
│   │   └── DecompositionWorker.cpp / .h  # Async on-demand child grid generation
│   ├── renderer
│   │   ├── Renderer.h                    # Abstract renderer interface
│   │   ├── BgfxRenderer.cpp / .h         # bgfx backend: window surface, shaders, floating origin
│   │   └── LODManager.cpp / .h           # Per-layer view distance and chunk budgets
│   ├── platform
│   │   ├── Window.cpp / .h               # GLFW window; exposes native handles
│   │   └── NativeWindowHandles.h         # Library-neutral window↔renderer seam
│   ├── simulation
│   │   ├── PhysicsSystem.cpp / .h        # Material-property-driven structural simulation
│   │   └── PropagationSystem.cpp / .h    # Upward damage propagation across composite layers
│   ├── io
│   │   ├── VoxImporter.cpp / .h          # .vox format import with layer assignment
│   │   └── VoxExporter.cpp / .h          # .vox format export with auto-chunking
│   └── plugins
│       └── ExamplePlugin.cpp / .h        # Reference plugin: feature generator + material def
├── demos                                # Progressive series of reference examples
│   └── 01-single-voxel                  # M2: single voxel in space (auto-orbit / free-cam)
│       └── main.cpp                      # Each demo/<NN-name>/main.cpp builds its own target
├── plugins                              # Runtime-loadable plugins, each a MODULE shared lib
│   ├── base-terrain/plugin.cpp          # Materials + terrain layer generator (the M3 world)
│   └── water/plugin.cpp                 # Removable: water material + sea-level feature generator
├── tests
│   └── LayerConfigTest.cpp               # Unit tests; link voxel-engine + GoogleTest
├── shaders                               # bgfx .sc shader sources + committed bytecode
│   ├── vs_voxel.sc / fs_voxel.sc         # Authored shaders (with varying.def.sc)
│   └── generated/                        # Per-backend bytecode headers (committed; see ARCHITECTURE §9)
├── include                               # Public API (propagated to engine consumers)
│   ├── plugin_api.h                      # Public plugin interface; flat callback registration
│   └── WorldCoord.h                      # Double-precision coordinate type; wraps dvec3
├── docs
│   └── ARCHITECTURE.md                   # Subsystem design, invariants, AI agent guidance
├── CMakeLists.txt
└── README.md
```

---

## Plugin API

Plugins register flat callbacks for named engine hooks rather than subclassing engine types. This keeps each plugin's contract visible and self-contained.

Registerable hooks (subject to revision as the API matures):

- **World generation** — Layer N procedural population
- **Feature generators** — Sub-structure stamps used by composition recipes (caves, dungeons, ore veins, etc.)
- **Material definitions** — Register new materials with property values and palette entries
- **Import/Export handlers** — Custom format support or extended `.vox` sidecar data
- **Simulation hooks** — React to material state changes, structural events, or player interactions
- **Layer lifecycle hooks** — Called when a layer chunk is created, decomposed, modified, or evicted

To create a plugin, implement the interface defined in `include/plugin_api.h` and load it via `PluginManager`. See `src/plugins/ExamplePlugin` for a worked example covering feature generator registration and material definition.

---

## Setup

The engine builds as a library (`voxel-engine`); demos and games are separate
executables that link it. The build is static by default — pass
`-DBUILD_SHARED_LIBS=ON` to produce a shared engine library instead.

The `demos/` directory holds a progressive series of reference examples, each a
standalone target named after its folder. Every `demos/<NN-name>/` with a
`main.cpp` is discovered automatically — `cmake --build build` builds them all,
or build one with `--target <NN-name>`. To add a demo, drop in a new
`demos/<NN-name>/main.cpp`; no CMake edits are needed.

The `plugins/` directory is discovered the same way: every `plugins/<name>/`
with a `plugin.cpp` is built as a runtime-loadable shared library into
`build/plugins/`. The `03-plugin-driven-world` demo loads them from there.

```bash
git clone <repository-url>
cd voxel-game-engine
cmake -B build
cmake --build build

# Run the first demo (single voxel in space).
# Single-config generators (Make/Ninja):     ./build/01-single-voxel
# Multi-config generators (Visual Studio):    ./build/Debug/01-single-voxel.exe

# Run the plugin-driven world (M4): terrain + materials come from disk-loaded
# plugins; press P to load/unload the water plugin and watch it flood/drain.
# Single-config:   ./build/03-plugin-driven-world
# Multi-config:    ./build/Debug/03-plugin-driven-world.exe

# Run the test suite
ctest --test-dir build
```

---

## Design Constraints

These constraints are enforced by the engine or its type system. They are documented here so contributors — human or AI — understand *why* they exist and do not work around them.

**`WorldCoord` is mandatory for all world-space positions.** Raw `double` or `float` must not be used for world-space coordinates anywhere in the engine or in plugins. `WorldCoord` wraps a double-precision vector and provides explicit conversion methods. This makes accidental float promotion a compile error. Single-precision floats silently lose sub-meter accuracy at kilometer scales.

**Layer ratios must be integers.** The child grid of a composite voxel must tile exactly. Non-integer ratios are rejected at startup, not silently rounded. Fix the config — do not add a workaround in the decomposition code.

**Every composite layer must have a recipe.** A composite layer with no registered recipe plugin is a startup error. It cannot be left as a runtime gap — there is no valid fallback behavior for an unrecipied composite voxel that a player interacts with.

**Decomposition must be deterministic.** The same recipe + seed must always produce the same child grid. This is what allows unmodified child chunks to be evicted and regenerated transparently. Do not introduce non-deterministic calls (`rand()`, `time()`, unordered container iteration without a stable order) into the decomposition pipeline.

**Dirty tracking is chunk-granular, not voxel-granular.** Per-voxel dirty tracking produces unmanageable save file sizes. The dirty flag marks whole chunks within a composite voxel. Chunk size is a tunable constant — set it conservatively.

**Immutable voxels do not propagate damage upward.** The upward propagation chain stops at an immutable layer boundary. Do not add propagation logic that crosses an immutable layer.

**Decomposition is one layer at a time.** A 100km composite voxel decomposes into its declared child layer (e.g. 10km), not directly into 1m terminal voxels. Each level in the chain is independently lazy. Do not shortcut the chain in the decomposition worker.

---

## Milestones

Development is organized into two phases. Phase 1 targets a minimum viable engine — something that renders a voxel world and accepts player interaction. Phase 2 targets a shippable 1.0 with the full feature set described in this document. Phase 2 milestones are listed as placeholders; each will be fleshed out in a design task before implementation begins.

### Phase 1 — Minimum Viable Engine

**M1 — Foundation**
- [x] `WorldCoord` type defined with `toLocalFloat()` conversion; raw float/double banned from world-space paths by convention
- [x] `LayerConfig` parser and validator with full startup error reporting
- [x] Single-layer project config working end-to-end (define one terminal layer, engine starts without errors)
- [x] CMake build clean on target platforms *(verified building clean on Linux/GCC, macOS/Clang, and Windows/MSVC via CI; bgfx.cmake pinned to release v1.143.9257-544, C++20 required by bx, `CMAKE_POLICY_VERSION_MINIMUM` set for older deps, Wayland dev packages added to the Linux CI job)*

**M2 — Basic Rendering**

> **Complete.** The full rendering pipeline is wired end-to-end: GLFW window → bgfx device → per-frame view/projection matrices via floating-origin → transient vertex buffers with palette-mapped colors → `bgfx::frame`. The `01-single-voxel` demo opens a live window, renders a single voxel at the world origin with the camera auto-orbiting it, and supports toggling into a WASD + mouse free-camera mode (press F). All tasks compile on Linux/GCC, macOS/Clang, and Windows/MSVC.

- [x] Floating-origin coordinate math in place (`WorldCoord::toLocalFloat`, camera-local submission helper)
- [x] Window + surface: GLFW-backed `src/platform/Window` creates a context-less window and feeds a library-neutral native handle into `bgfx::Init::platformData`
- [x] bgfx device init wired to the live window (single-threaded mode). Resize handling via `bgfx::reset` in `setViewport`, called each frame when the framebuffer size changes
- [x] Shader toolchain: vertex-color vs/fs compiled by `shaderc` to committed per-backend bytecode (SPIR-V/GLSL/ESSL/DXBC/Metal; opt-in `-DVOXEL_BUILD_SHADERS=ON` regeneration); `bgfx::ProgramHandle` built via `createEmbeddedShader`
- [x] Per-frame camera transform: view matrix built from pitch/yaw at the floating origin; projection matrix via `bx::mtxProj`; both submitted via `bgfx::setViewTransform` each frame
- [x] Render loop wired into `main`: init renderer → poll window events → update camera → draw → `bgfx::frame` → clean shutdown on window close or ESC
- [x] Renderer reads a flat voxel array and produces a visible window (`BgfxRenderer::renderWorld` iterates non-empty voxels and calls `drawVoxel` for each)
- [x] Palette-based material colors rendering correctly: 16-color ABGR palette indexed by `Voxel::material.palette_index`; per-voxel color applied via transient vertex buffers each frame
- [x] Basic free-camera movement (keyboard/mouse) for development/testing: WASD + Space/Shift to move, mouse to look; press F to toggle between free-camera and auto-orbit
- [x] **Demo — Single voxel in space:** one voxel rendered at the world origin with the camera auto-orbiting it; press F to switch to free-camera and explore from any angle

**M3 — World and Chunk Management**

> **Complete.** `World` gained a chunked terminal-layer mode: a `ChunkStore` (`unordered_map<ChunkCoord, Chunk>`) populated on demand by the registered layer generator and evicted as the camera moves. The example plugin's generator is now a deterministic value-noise heightmap (pure function of world x/z — adjacent chunks share a seamless surface and reload identically). `LODManager` reads each layer's `view_distance_chunks`, computes the desired chunk set as a column band around the camera, and drives eviction with a hysteresis margin. Per-chunk static meshes (face-culled, 32-bit indexed) are built on load and submitted via a floating-origin model transform, replacing per-voxel draw calls for streaming. The `02-streaming-terrain` demo flies a free camera over the heightmap with chunks streaming in/out within a bounded budget. The full finite-grid path and the `01-single-voxel` demo are unchanged.

- [x] Chunked terminal-layer world with configurable chunk size (`World(const LayerDef&)` + `ChunkStore`; `chunk_size_voxels` from `LayerConfig`; coord math in `src/world/ChunkCoordMath.h`)
- [x] Chunk load/unload within a view distance budget (`World::loadChunk`/`unloadChunk` driven by `LODManager` desired-set + hysteresis eviction; loads budgeted per frame in the demo)
- [x] Simple procedural generation for a single terminal layer: deterministic value-noise heightmap in the example plugin (no `rand`/`time`/static state; verified chunk-size-independent by test)
- [x] `LODManager` stub in place with per-layer view distance config read correctly (`src/renderer/LODManager.{h,cpp}` reads `LayerDef::view_distance_chunks`; column-band `desiredChunks`, `shouldEvict`)
- [x] **Demo — Streaming terrain flythrough:** `02-streaming-terrain` — free-camera flight over a procedurally generated single-layer heightmap whose chunks stream in and out around the camera within the view-distance budget

**M4 — Plugin System**

> **Complete.** The flat callback API (`include/plugin_api.h`) and the `PluginManager` registries drive a fully plugin-built world. Two plugins are now built as real shared libraries under `plugins/` and loaded from disk at runtime: `base-terrain` (materials + the `terrain` layer generator — the M3 look) and `water` (a `water` material + a feature generator that floods empty voxels up to a fixed sea level). `PluginManager` gained per-plugin runtime unload: every registration is tagged with its owning plugin and torn down — before the library is closed, so no callback can dangle — on `unloadPlugin`. The `03-plugin-driven-world` demo loads the base plugin from disk and toggles the water plugin on/off live (press P), regenerating resident chunks so flat water appears and disappears in place; with water unloaded the world is byte-for-byte the base terrain. Direct unit tests cover the disk-load, error, and teardown paths.

- [x] `PluginManager` loads plugins from disk: `loadPlugin` / `loadPluginsFromDirectory` via `LoadLibrary`/`dlopen`, resolves `voxel_plugin_init`, and calls it with a `PluginContext` (registration lambdas wired in `buildContext`)
- [x] Plugin **unloading**: `unloadPlugin(PluginId)` erases all of a plugin's registry entries (every record carries an internal owner id, set around its `init` call) **before** `FreeLibrary`/`dlclose`, so a callback can never point into unloaded code; a failed/non-zero `init` rolls back any partial registrations the same way
- [x] `plugin_api.h` stable for terminal-layer hooks: material registration, layer generator, feature generator, voxel modification hook *(the `on_voxel_modified` surface exists but is not yet fired anywhere — voxel modification lands in M5; chunk-lifecycle hooks are fired by the M3 streaming loop; feature generators are applied after the base layer generator in the M4 chunk path)*
- [x] Example plugin registers materials (stone, grass) and a layer generator (terrain); documented as the canonical reference (`src/plugins/ExamplePlugin`, architecture.md §8). The disk-loaded `plugins/base-terrain` is its shared-library counterpart with identical generation math
- [x] Plugin load errors reported clearly at startup (cannot-open, missing `voxel_plugin_init`, non-zero init return) — exercised end-to-end by the disk-load unit tests
- [x] Build the example plugin as a real `.so`/`.dll` and load it through `loadPlugin` / `loadPluginsFromDirectory`, exercising the disk-load + error-reporting paths (`plugins/base-terrain`, `plugins/water`)
- [x] Direct unit tests for the plugin system: load-from-disk success/failure, missing-symbol and non-zero-init error paths (with rollback), per-plugin unload teardown, duplicate-material overwrite warning (`tests/PluginManagerTest.cpp`, fixtures under `tests/fixtures/`)
- [x] **Demo — Plugin-driven world:** `03-plugin-driven-world` — a world whose materials and terrain come entirely from disk-loaded plugins; press P to load/unload the `water` plugin at runtime and watch its standing water appear/disappear, with the world reverting exactly to the base-terrain (M3) output on removal

**M5 — Player Interaction**

> Player interaction turns the read-only streaming world (M3/M4) into a writable one. The foundation it adds is the first **world-space single-voxel accessor** on the chunked `World` — until now the chunked path addressed voxels only by per-chunk local index or by whole-chunk load/unload, with no way to read or write the voxel at an arbitrary `WorldCoord`. Picking, editing, and collision all build on that accessor. M5 is also the first milestone to **fire the `on_voxel_modified` hook** (registered in M4, dormant until now) and the first to **write game state to disk**. Collision here is deliberately lightweight kinematic terminal-voxel AABB resolution — the material-property-driven `PhysicsSystem` (structural load, collapse, propagation) is M11, not this.

*World-space voxel access — the shared foundation*
- [x] Global voxel-coordinate math in `ChunkCoordMath.h`: a 64-bit `VoxelCoord` plus `worldToVoxel`, `voxelOrigin`/`voxelCenter`, and `voxelToChunkLocal`/`chunkLocalToVoxel` conversions between `WorldCoord`, a global integer voxel coordinate, and `(ChunkCoord, local x/y/z)`; double-only and `floor`-based to match `worldToChunk` (correct on the negative side of the origin), covered by `tests/WorldVoxelAccessTest.cpp`
- [x] World-space single-voxel accessors on the chunked `World`: `getVoxel(WorldCoord)` returns the resolved voxel or `Voxel::empty()` when the owning chunk is not resident; `setVoxel(WorldCoord, const Voxel&)` writes only when the chunk is resident and returns whether it did (no load-on-demand — the player edits already-streamed voxels), kept distinct from the finite-grid `getVoxel(int,int,int)`

*Place and remove terminal-layer voxels*
- [x] Voxel raycast by DDA grid traversal, in double precision, from the eye along the camera look vector, walking chunk-to-chunk across the resident `ChunkStore`; returns the first solid terminal voxel hit, the hit-face normal, and the adjacent empty cell, bounded by a max reach distance (`src/world/VoxelRaycast.{h,cpp}`, Amanatides & Woo; cells in non-resident chunks read as empty; covered by `tests/VoxelRaycastTest.cpp`)
- [x] Remove clears the hit voxel to `Voxel::empty()`; place writes the selected material into the adjacent empty cell, guarded against placing into the cell the camera occupies (the full player-AABB guard arrives with the collision group below)
- [x] Build material selected from the loaded-plugin material registry (`PluginManager::materials()`); keys 1-9 pick a registered material in the `04-build-break-persist` demo
- [x] Re-mesh the edited chunk after each modification via `ChunkMesh::build`. Only the owning chunk is rebuilt: the mesher always emits opaque border faces (no cross-chunk culling, ARCHITECTURE §9), so the neighbor's coincident face is already present and an opaque seam edit needs no neighbor rebuild
- [x] Fire the `on_voxel_modified` hook (`PluginManager::voxelModifiedHooks()`) on every edit with the old/new `Voxel` and world position — activating the surface declared in `plugin_api.h` and registered in M4 but never previously fired
- [x] Mouse-button input (left break / right place) added to the demo input path; targeted-voxel wireframe highlight (`BgfxRenderer::drawVoxelHighlight`) and a centered crosshair (`setCrosshair`, bgfx debug text)

*Dirty tracking at chunk granularity*
- [x] Chunk-granular dirty flag on `Chunk` (`dirty()`/`markDirty()`/`clearDirty()`), set by the world-space `World::setVoxel` and queryable via `isChunkDirty`/`dirtyChunkCoords`/`clearChunkDirty`; generation through `Chunk::data()` stays clean, only post-generation edits dirty a chunk (`tests/ChunkDirtyTest.cpp`)
- [x] LOD eviction made dirty-aware: the `04-build-break-persist` eviction pass skips dirty chunks (`!world.isChunkDirty(c)`) so edits are never silently lost when the camera moves away. The persistence group below extends this to save-then-evict (dirty chunks are written to disk, then allowed to leave memory)

*Persistence — save/load of dirty chunks*
- [x] Internal chunk save format (`src/io/ChunkPersistence.{h,cpp}`): `VXCK` magic + version header carrying the world identity (`voxel_size_m`, `chunk_size_voxels`) and `ChunkCoord`, then a deduplicated material **palette + run-length** encoding of the grid; one `c_<x>_<y>_<z>.vxc` file per chunk. The engine's own save format, distinct from the M7 `.vox`/`.vxe` interop formats
- [x] Save path: `WorldSave::saveDirtyChunks` writes every dirty chunk to a per-world save directory and clears its dirty flag; the `04-build-break-persist` demo saves on quit and save-then-evicts dirty chunks leaving the view budget. The world identity is stamped into each file so a save can be matched back to its layer config
- [x] Load path: on chunk load the demo prefers a saved chunk (`hasChunk`/`tryLoadChunk` → `World::insertChunk`) over running the generator; clean chunks still regenerate deterministically on cache miss, while a loaded chunk is authoritative and does **not** re-run the generator or feature generators
- [x] Round-trip identity verified: `tests/ChunkPersistenceTest.cpp` checks codec and on-disk save/load are byte-for-byte equal to the in-memory grid, and that identity-mismatch, garbage, and truncated files are rejected without crashing

*Collision against terminal-layer voxels*
- [x] Kinematic player body: `WorldCoord` position plus AABB extents, gravity, jump, and grounded state — a walk mode (press G) distinct from the free-fly camera, built on the world-space voxel accessor (`04-build-break-persist`)
- [x] Swept, axis-separated AABB-vs-voxel resolution against solid terminal voxels (`src/world/VoxelCollision.{h,cpp}`): substepped (≤ half a voxel per step) so it cannot tunnel at speed, resolving each axis independently so the box slides along surfaces; cells in non-resident chunks read as empty

*Tests*
- [x] Unit tests covering: global voxel-coordinate round-trip including negative coordinates and chunk seams (`WorldVoxelAccessTest`); DDA raycast hits and face normals on a known grid (`VoxelRaycastTest`); AABB collision resolution — no tunneling, correct grounding, wall stop/slide, ceiling (`VoxelCollisionTest`); dirty-flag set/clear (`ChunkDirtyTest`); and save/load round-trip equality including evict-then-reload identity (`ChunkPersistenceTest`)

- [x] **Demo — Build, break, and persist:** `04-build-break-persist` — press G to walk the streamed world under gravity with terminal-voxel collision, place and remove voxels with the mouse (targeted voxel highlighted, crosshair shown), then quit and relaunch to confirm modified chunks were saved and reload identically while untouched terrain regenerates from the plugin generator

**M6 — Multi-Layer Support**
- [ ] Two-layer project config working (one composite layer above one terminal layer)
- [ ] Composite voxels render as solid atomic blocks before decomposition
- [ ] On-demand decomposition triggers correctly when player interacts with a composite voxel
- [ ] `DecompositionWorker` running on a thread pool with async pop-in; main thread never blocked
- [ ] Decomposition determinism verified by test: same seed produces identical child grid across multiple runs
- [ ] Immutable layer mode working: renders and collides, no decomposition, no persistence
- [ ] **Demo — Decompose on approach:** a two-layer world where a large composite voxel renders as a solid block from afar and decomposes into its terminal child grid on demand as the player approaches, alongside an immutable backdrop layer that renders and collides but never decomposes

**M7 — Voxel Editor Interoperability**
- [ ] `.vox` import assigns content to a specified layer at a specified world anchor
- [ ] `.vox` export with automatic chunking for volumes larger than 256³
- [ ] Lossy export fallback (extended data dropped, warning logged) working correctly
- [ ] Tested against a real MagicaVoxel-exported file
- [ ] **Demo — MagicaVoxel round-trip:** import a real `.vox` model to a chosen layer and world anchor, view and edit it in-engine, then export back to `.vox` — exercising auto-chunking for large volumes and the logged lossy fallback when extended data is dropped

---

### Phase 2 — 1.0 Release

> Phase 2 milestones are placeholders. Each will have a dedicated design task to produce a detailed plan before implementation begins. Order and scope are subject to revision after Phase 1 is complete.

**M8 — Material Property System**
- [ ] Design task: finalize material property schema and physics integration contracts
- [x] Full material property struct on voxels
- [x] Material definitions registered via plugin API
- [ ] Simulation systems (mining resistance, etc.) driven by properties rather than IDs
- [ ] **Demo — Material matters:** mine and interact with several materials whose differing `hardness`, `density`, and `structural_strength` produce visibly different behavior, all driven by plugin-registered property values rather than block IDs

**M9 — Composition Recipes and Feature Generators**
- [ ] Design task: recipe schema, feature generator interface, hierarchical seed parameter passing
- [ ] Recipe system implemented and referenced from composite voxel types
- [ ] Feature generator plugin hook live; example cave generator plugin written
- [ ] Hierarchical seed parameters passed from parent recipe to child recipe
- [ ] **Demo — Recipe-built voxel:** decompose a composite voxel whose recipe stamps feature overlays (e.g. a cave network and ore veins via the example feature-generator plugin) and biases child materials, with a parent seed visibly constraining what the children generate

**M10 — Cascading Multi-Layer Decomposition**
- [ ] Design task: cache eviction policy, memory budget across deep layer stacks
- [ ] Full N-layer decomposition chain working
- [ ] Clean chunk eviction and deterministic regeneration on cache miss verified by test
- [ ] **Demo — Drill to the core:** descend through the full N-layer chain (e.g. continental → regional → local → terrain) one layer at a time, then revisit a previously evicted region to confirm it regenerates identically

**M11 — Physics and Upward Damage Propagation**
- [ ] Design task: propagation algorithm, performance budget, immutable boundary behavior
- [ ] Structural strength aggregation from child voxels to parent composite
- [ ] Collapse events firing and cascading correctly
- [ ] Propagation confirmed to stop at immutable layer boundaries
- [ ] **Demo — Structural collapse:** hollow out a composite structure until its aggregated structural strength fails, triggering a collapse that cascades to neighbors and visibly stops at an immutable layer boundary

**M12 — Fluid and Thermal Simulation**
- [ ] Design task: fluid and heat transfer algorithms, interaction with porosity and thermal conductivity properties
- [ ] Fluid flow driven by `porosity` material property
- [ ] Heat transfer driven by `thermal_conductivity` material property
- [ ] **Demo — Flow and heat:** release a fluid that flows through porous materials and is blocked by low-`porosity` ones, plus a heat source that spreads at rates set by each material's `thermal_conductivity`

**M13 — Polish and Release**
- [ ] Design task: identify gaps between Phase 1 implementation and full architecture spec
- [ ] ARCHITECTURE.md fully reflects implemented behavior (not just intended behavior)
- [ ] Example plugin suite covers all major hook types
- [ ] Performance profiling pass on decomposition, chunk management, and rendering
- [ ] 1.0 tag

---

## Further Reading

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) is the primary reference for anyone — human or AI — doing non-trivial work on the engine. It covers:

- The *why* behind every major design decision, not just the what
- A full subsystem dependency map defining which systems may talk to which
- The complete list of hard invariants and common mistakes
- A section written directly at AI coding agents with explicit rules and a proceed-vs-ask heuristic

The README describes what the engine is. ARCHITECTURE.md describes why it is that way and how to work inside it safely.

---

## License

MIT License. See LICENSE for details.

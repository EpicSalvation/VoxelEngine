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
| `hardness` | Resistance to removal/destruction |
| `palette_index` | Maps to a visual material definition (color, texture, PBR params) |

Mods that add new materials define property values rather than special-case code. Physics, fluid, and voxel-removal systems respond to properties, not IDs. A new volcanic rock mod does not require changes to lava flow logic — it just declares material properties that the existing fluid system already understands.

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
│   ├── water/plugin.cpp                 # Removable: water material + sea-level feature generator
│   └── layered-world/plugin.cpp         # M6: blocks/terrain/backdrop generators for three layers
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

The build is verified on Linux (GCC) — `cmake -B build && cmake --build build`
configures and compiles cleanly from a fresh checkout, and `ctest --test-dir
build` passes. macOS/Clang and Windows/MSVC are likewise supported.

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

# Run build/break/persist (M5): fly down, press G to drop into the walking
# player (gravity + collision), left/right-click to break/place voxels (1-9
# pick the material), then quit (ESC) and relaunch to confirm your edits
# reloaded. Edits are saved to a "voxelsave/" directory beside the working
# directory you launch from.
# Single-config:   ./build/04-build-break-persist
# Multi-config:    ./build/Debug/04-build-break-persist.exe

# Run decompose-on-approach (M6): a three-layer world — composite blocks over a
# terminal terrain child layer, beside an immutable backdrop. Fly toward the
# coarse blocky terrain and watch macro voxels decompose into fine 1 m terrain;
# press G to walk with collision across all layers.
# Single-config:   ./build/05-decompose-on-approach
# Multi-config:    ./build/Debug/05-decompose-on-approach.exe

# Run MagicaVoxel round-trip (M7): a 4×4×4 coloured cube is auto-generated as
# test_model.vox and imported into an "editor" layer; fly around, edit with
# place/remove, then press E to export back to output.vox (auto-chunking and
# lossy-property warning logged to the terminal if applicable).
# Single-config:   ./build/06-magicavoxel-round-trip
# Multi-config:    ./build/Debug/06-magicavoxel-round-trip.exe

# Run the arena platformer (M7b): spawn in a five-layer 500 m walled arena.
# Gold key stakes are imported .vox models; walk into each to collect it (G to
# enable walk mode), then reach the goal totem to win. Press P to toggle lava
# hazards on the platforms, E to export your built region to arena-export.vox.
# Single-config:   ./build/07-arena-platformer
# Multi-config:    ./build/Debug/07-arena-platformer.exe

# Run material matters (M8): a flat strata world — soft topsoil over stone, iron,
# and diamond on an indestructible bedrock floor. Aim down and hold left mouse to
# mine; harder materials take visibly longer (the highlight ramps red) and bedrock
# never clears. The HUD reads out the targeted voxel's hardness/density/structural
# strength; right mouse places the selected material (1-6).
# Single-config:   ./build/08-material-matters
# Multi-config:    ./build/Debug/08-material-matters.exe

# Run the test suite
ctest --test-dir build
```

Full controls for `04-build-break-persist`: **WASD** move, **mouse** look,
**F** toggles the mouse cursor, **G** toggles walk/fly, **Space/Shift** fly
up/down (or **Space** to jump while walking), **left/right mouse** break/place,
**1**–**9** select the build material, **ESC** quits.

Controls for `05-decompose-on-approach`: **WASD** move, **mouse** look,
**Space/Shift** fly up/down (or **Space** to jump while walking), **G** toggles
walk/fly (collision across all layers), **F** toggles the mouse cursor, **ESC**
quits. Fly toward the coarse blocky terrain to decompose it into fine detail.

Controls for `06-magicavoxel-round-trip`: **WASD** move, **mouse** look,
**Space/Shift** fly up/down, **left/right mouse** break/place, **1**–**9**
select palette material, **E** export the current editor layer to `output.vox`,
**F** toggles the mouse cursor, **ESC** quits.

Controls for `07-arena-platformer`: **WASD** move, **mouse** look, **G** toggles
walk/fly (cross-layer collision + gravity), **Space** jump (walk) or fly up,
**Shift** fly down, **left/right mouse** break/place, **1**–**9** select
material, **P** toggles lava hazards on platforms, **E** exports the detail layer
to `arena-export.vox`, **F** toggles the mouse cursor, **ESC** quits. From the
floor spawn, switch to walk mode and head north to the stone staircase, then jump
up its steps onto the start pad (walk-mode collision has no step-up, so each 1 m
riser is a jump). Walk into gold key stakes to collect them — an on-screen
counter tracks your progress — and reach the goal totem with all four keys to win.

Controls for `08-material-matters`: **WASD** move, **mouse** look, **G** toggles
walk/fly, **Space/Shift** up/down (fly) or jump (walk), **hold left mouse** mine
the targeted voxel (harder materials take longer; bedrock never clears), **right
mouse** place the selected material, **1**–**6** select material, **F** toggles
the mouse cursor, **ESC** quits. Aim straight down and dig through the strata to
feel each material's hardness; the HUD shows the targeted voxel's hardness,
density, and structural strength.

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

**M6 — Multi-Layer Support** ✅

> Multi-layer support is the first milestone to use more than one entry of the `LayerConfig` stack at runtime. Everything through M5 ran a single terminal `World`; M6 turns that into a **layer stack** — a composite layer of large atomic blocks above a terminal child layer, plus an independent immutable backdrop layer — and adds the **on-demand decomposition** that turns a composite voxel into its child layer's voxel grid as the player approaches. The `VoxelMode` enum and `LayerDef::decompose_to` already exist and are validated (`LayerConfig::validate`, architecture.md §2/§3); M6 is the first milestone where they drive behavior. The three subsystems named in the project structure but not yet present — `Layer`, `MacroVoxel`, and `DecompositionWorker` — are created here. No `plugin_api.h` change is needed: each layer is populated by a generator registered under its layer name (the existing per-layer hook), and the composition-**recipe** hook is deferred to M9 — for M6 a composite voxel decomposes by running its child (terminal) layer's generator over the macro-voxel's subvolume.

*Layer stack — the shared foundation*
- [x] Extract per-layer chunk management out of `World` into `src/world/Layer.{h,cpp}`: the `ChunkStore`, generator, and world-space `getVoxel`/`setVoxel` (the M3/M5 behavior) become a `Layer`, leaving the single-terminal-layer demos and tests behaving identically. `World` keeps the single-layer chunked API by forwarding to a *primary* layer (the terminal layer)
- [x] Make `World` a multi-layer container built from the full `LayerConfig` (one `Layer` per `LayerDef`, looked up by `layer(name)`), resolving each composite layer's `decompose_to` to its child `Layer` via `childLayer()`. Adds `World::anySolidAt(pos)`, a cross-layer solid query sampling every layer at its own scale (used by collision so the player meets composite + immutable + terminal voxels)
- [x] Layer-aware coordinate math: `ChunkCoordMath.h` gained `layerRatio`, `childVoxelMin`, and `childToParentVoxel` for the parent-voxel → child-layer-subvolume mapping (integer `voxel_size_parent / voxel_size_child` ratio), plus `VoxelCoordHash`; existing world↔voxel/chunk conversions already scale by `voxel_size_m`
- [x] Drive `LODManager` per layer: each resident layer streams within its own `view_distance_chunks` budget independently (demo 05 streams `blocks`, `backdrop`, and decomposed `terrain` each on their own budget)

*Composite voxels & atomic rendering*
- [x] `src/world/MacroVoxel.h`: `DecompositionState` tracks each macro voxel's state (none / pending / decomposed) so a decomposed macro is not re-decomposed while resident, an in-flight one is not re-enqueued, and an undecomposed one renders as a solid atomic block
- [x] Render undecomposed composite-layer chunks through the existing mesher: `BgfxRenderer::renderChunk(mesh, origin, voxel_size_m)` applies a `bx::mtxSRT` floating-origin model transform scaled by the layer's voxel size, so composite chunks mesh as large solid blocks at the correct world scale
- [x] Swap representations on decomposition: when a macro voxel's child grid becomes resident the demo clears the macro voxel (the coarse block stops rendering and contributing to collision) and re-meshes the composite chunk; on eviction the child chunks are dropped and the macro voxel returns to the atomic state (`DecompositionState::clear`)

*On-demand decomposition*
- [x] `src/world/DecompositionWorker.{h,cpp}`: a `DecompositionJob` (macro coord + child chunks + child chunk/voxel size + generator) produces the child layer's voxel grid for the subvolume by running the child generator. Determinism is inherited from the pure generator (no `rand`/`time`/unordered-container iteration, architecture.md §4); the worker adds none, verified by repeated and concurrent runs
- [x] Run decomposition on a thread pool with async pop-in: `enqueue` queues a job, `drain` returns completed child grids on the main thread (never blocking), and the demo inserts them into the child `Layer`'s `ChunkStore`. The atomic block keeps rendering until the result arrives
- [x] Trigger decomposition on approach: each frame the demo enqueues undecomposed macro voxels within `kDecomposeRadiusM` of the camera (`DecompositionState::markPending` claims each exactly once). Direct-interaction picking stays on the terminal layer; composite picking is deferred to M9 recipes

*Immutable layer*
- [x] Immutable layer mode: chunks generated once and retained — generation leaves them clean, so they never dirty, are never a persistence candidate, and are never decomposed (the demo's eviction path drops them directly with no save)
- [x] Collide against immutable voxels: `World::anySolidAt` samples the immutable layer, so the walking player (`VoxelCollision::moveAABB`) stands on the backdrop as well as terminal voxels. Picking/editing intentionally stays on the terminal layer (an immutable layer is, by definition, not editable)
- [x] Submit immutable-layer geometry through the same scaled mesher at its own `voxel_size_m` (demo 05 renders the 2 m backdrop via `renderChunk(..., backdrop->voxelSizeM())`)

*Tests*
- [x] Decomposition determinism (`tests/DecompositionTest.cpp`): the same (generator, macro-voxel) yields a byte-for-byte identical child grid across repeated and concurrent runs, plus `DecompositionState` pending/decomposed bookkeeping
- [x] Two-layer config validates (composite over terminal, integer ratio, valid `decompose_to`) and a malformed stack is rejected (`LayerConfigTest`); `tests/MultiLayerWorldTest.cpp` covers layer ordering, terminal-as-primary, `childLayer` resolution, and `anySolidAt`
- [x] Layer-aware coordinate round-trip including the parent-voxel → child-subvolume mapping (`ChunkCoordMathTest`); an immutable-layer chunk never reports dirty after generation or after a coincident terminal-layer edit (`MultiLayerWorldTest`)

- [x] **Demo — Decompose on approach:** `05-decompose-on-approach` — a three-layer world (composite `blocks` over a terminal `terrain` child layer, beside an immutable `backdrop`); large composite voxels render as solid blocks from afar and decompose into their terminal child grid with async pop-in as the player flies closer, while the immutable layer renders and collides but never decomposes. Run/controls in the demo header and the Setup section

**M7 — Voxel Editor Interoperability** ✅

> M7 adds the two classes named in the project structure but not yet present — `VoxImporter` and `VoxExporter` — and wires them into the engine as built-in handlers for the `.vox` extension. The plugin import/export hooks (`register_importer`, `register_exporter`) and the `palette_index` bridge field already exist; M7 is the first milestone where they drive real file I/O. `.vox` is a single-layer, palette-indexed format with a 256³-per-object limit; the importer reassembles multi-object files at a caller-supplied layer and world anchor, while the exporter partitions oversized regions into multiple anchored objects. Extended material properties are out of scope for `.vox` — when they are present the engine falls back to palette-only export and logs a warning, a path that must be exercised by the demo and covered by a test.

*`.vox` binary format parser* ✅
- [x] `src/io/VoxImporter.{h,cpp}`: parse the MagicaVoxel `.vox` chunk tree (MAIN, SIZE, XYZI, RGBA); extract per-voxel palette indices and the 256-entry RGBA palette; fall back to the built-in MagicaVoxel default palette when the file contains no RGBA chunk
- [x] Parse transform nodes (nTRN) to reconstruct world-relative positions for multi-model `.vox` files so volumes spanning >256³ are assembled correctly at import time from their per-object anchor offsets
- [x] `VoxImporter::load(path, layer, anchor)`: populates `palette_index` on each target voxel and initializes other material properties from the palette entry's registered default property set (architecture.md §10 — no inference from color values). Also installs the file's authored RGBA colors into the engine's shared visual palette (`src/renderer/Palette.h`) for the indices the model uses, so imported voxels render with — and re-export to — the colors they were drawn with

*`.vox` binary format writer* ✅
- [x] `src/io/VoxExporter.{h,cpp}`: serialize a layer region to `.vox` binary; always emits a scene graph so nTRN center offsets survive re-import correctly; single SIZE+XYZI+RGBA+scene-graph for regions ≤256 voxels in every axis
- [x] Auto-chunking: for regions exceeding 256 voxels in any axis, partition into 256³ sub-volumes and encode each chunk's world offset via nTRN transform nodes so MagicaVoxel and other compliant readers reconstruct the correct layout on re-import
- [x] Palette serialization: collect the distinct `palette_index` values present in the exported region and write them as an RGBA chunk using the engine's **live visual palette** (`src/renderer/Palette.h`) — the exact colors the voxels render with, alpha preserved — so a file's colors survive import → edit → export instead of being remapped to an unrelated default table; indices unused by any voxel in the region keep a neutral gray. The render palette is a runtime 256-entry table (seeded from the 16-color base, populated per-import) rather than a fixed 16-color set

*Lossy export fallback and engine wiring*
- [x] Wire `VoxExporter` as the built-in fallback in the plugin dispatch path: if no plugin has registered a `.vox` exporter, the engine invokes `VoxExporter` directly, exporting only `palette_index` values and emitting `LOG_WARN("extended voxel properties dropped; register an exporter plugin to preserve them")`; the warning triggers whenever any voxel's non-palette properties differ from their defaults *(implemented in `Engine::exportVox`; warning captured by `Log::setWarnHandler`; `src/core/Logger.{h,cpp}` provides the test-redirectable warn channel)*
- [x] Register `VoxImporter` and `VoxExporter` as built-in handlers in `Engine::init()` via the existing `register_importer` / `register_exporter` hooks (no `plugin_api.h` change required); built-in handlers have lower priority than any plugin-registered handler for the same extension *(`PluginManager::registerBuiltinHandlers()` inserts marker entries with `isBuiltin=true`; `Engine::init()` calls it; dispatch in `importVox`/`exportVox` skips built-in entries when a plugin handler exists)*
- [x] Expose `Engine::importVox(path, layerName, anchor)` and `Engine::exportVox(layerName, region, path)` as the public call surface used by demos and tests *(added to `src/core/Engine.{h,cpp}`; engine stores `PluginManager*` and `World*` set by `Engine::init()`)*

*Tests*
- [x] `tests/VoxImportExportTest.cpp`: round-trip a minimal hand-crafted `.vox` byte sequence and verify `palette_index` values survive import → export → re-import unchanged
- [x] Import into a named layer at a non-zero world anchor and verify each voxel's world-space position matches the anchor offset plus the in-file coordinate
- [x] Import a two-object `.vox` (two SIZE+XYZI chunks with distinct nTRN offsets) and verify both objects are placed at the correct world positions relative to the anchor
- [x] Export a 300×1×1 layer region and verify the output contains exactly two objects with nTRN offsets that split at the 256-voxel boundary (auto-chunking coverage)
- [x] Confirm the lossy warning is emitted (capture log output) when exporting a layer whose voxels carry non-default extended properties and no plugin exporter is registered (`VoxLossyWarning.EmitsWarnWhenExtendedPropertiesPresent`; companion test verifies no spurious warning when only `palette_index` is set)
- [x] Verify a `.vox` file's authored colors survive import → render palette → export → re-parse, including a translucent (alpha < 255) palette entry (`VoxImportExport.PreservesAuthoredColorsThroughRoundTrip`); a gtest listener resets the shared visual palette before each test for isolation (`tests/TestPaletteReset.cpp`)

- [x] **Demo — MagicaVoxel round-trip:** `06-magicavoxel-round-trip` — generates a 4×4×4 coloured cube as `demos/06-magicavoxel-round-trip/assets/test_model.vox` if absent; on startup imports it to the `editor` layer at world origin via `Engine::importVox`; renders it through the existing mesher; supports place/remove editing (same controls as prior demos); press **E** to export the current `editor` layer back to `output.vox` with terminal log lines confirming whether auto-chunking triggered and whether the lossy-property warning fired

**M7b — MVP Capstone: Arena Platformer** ✅

> M7b adds no new engine subsystems — it is the integration milestone that proves the Phase 1 MVP by building a small but complete **3D platformer** inside a 500 m arena, deliberately exercising every M1–M7 capability at once. The arena is a five-layer stack: a single 500 m **immutable** voxel whose top face is the floor, a 20 m **immutable** perimeter wall with landmark towers, 10 m **composite** platforms that decompose on approach into the 1 m **terminal** build layer, and 2 m **immutable** props between. Decomposition stays single-step (a 10 m platform → its 1 m child grid, per M6); the deep lazy 500→20→10→2→1 cascade is intentionally left to M10. The objective is *collect-the-keys-then-reach-the-goal*: the scattered keys and the goal totem are MagicaVoxel `.vox` models imported with their authored colors. All new code lives in one new demo (`07-arena-platformer`) and two new plugins (`arena`, `hazards`) — no engine or `plugin_api.h` change is required.

*Arena world — layer stack, generation, and materials (M1, M3, M4)*
- [x] Five-layer `LayerConfig` (`foundation` 500 m immutable / `ramparts` 20 m immutable / `terraces` 10 m composite → `detail` / `props` 2 m immutable / `detail` 1 m terminal); integer ratios 25:1, 2:1, 5:1, 2:1 validated at startup, demonstrating the validator on a real multi-scale stack
- [x] New `arena` plugin registers a layer generator per layer (floor slab, perimeter wall + towers, coarse terraces, small props, fine detail) and the arena materials (stone, grass, hazard-lava, goal-gold), all deterministic (no `rand`/`time`/unordered iteration, architecture.md §4)
- [x] The 1 m `detail` layer streams within its `view_distance_chunks` budget around the player (a 500 m arena at 1 m is ~125 M voxels — streaming is mandatory, not optional); every layer drives `LODManager` on its own budget
- [x] A feature generator stamps key pickups and platform decorations onto the generated platforms (the M4 feature-generator hook, applied after the base layer generator)

*Multi-scale world — decomposition, modes, and collision (M6)*
- [x] Drive single-step decomposition on approach in the demo: `terraces` macro voxels within a radius are enqueued to `DecompositionWorker` and drained on the main thread into the `detail` layer (the proven M6 path); distant terraces render as 10 m blocks via the scaled `renderChunk`
- [x] Cross-layer collision via `World::anySolidAt`: the kinematic player stands on the 500 m floor, the 20 m ramparts, the 2 m props, and 1 m platforms simultaneously, each sampled at its own scale
- [x] Immutable floor / walls / props render and collide but never decompose, dirty, or persist; only `detail` edits are persistent

*Platformer mechanics (M2, M5)*
- [x] Walk mode with gravity, jump, and grounded state (the M5 kinematic body) is the core traversal; swept axis-separated AABB collision lands the player on platforms and stops them at walls
- [x] Free-fly camera (F) to survey the course; floating-origin submission keeps sub-meter precision 250 m+ from the arena center (M2)
- [x] Build/break on the `detail` layer (raycast + `setVoxel`, 1–9 material select) lets the player bridge gaps or make shortcuts; edited chunks re-mesh and fire `on_voxel_modified`. Placing into a not-yet-resident detail chunk (e.g. the air above a platform, whose top sits on a chunk boundary) creates the chunk on demand so building upward works
- [x] A stone starter staircase, authored in the arena plugin's `terraces`/`detail` generators (coarse superset carved to its 1 m step shape), climbs from the floor to the start pad so the floor-spawned player can reach the platform network
- [x] Player edits persist across runs (chunk-granular dirty tracking + `WorldSave`), so a built bridge is still there on relaunch

*Game objective — collect keys, reach the goal (demo logic + M7)*
- [x] Keys and the goal totem are imported `.vox` models (`Engine::importVox`) placed at world anchors and rendered with their authored palette colors (the M7 color round-trip)
- [x] Collect-then-finish loop: walking through a key's trigger volume collects it; reaching the goal totem with all keys collected logs victory; falling off or touching a hazard respawns the player at the start. An on-screen HUD counter (bgfx debug-text overlay) shows keys collected out of four and the win prompt
- [x] Live `hazards` plugin load/unload (P) adds/removes lava pools at runtime, reverting the arena exactly when unloaded (the M4 live-toggle pattern)
- [x] Export (E) the player-built region of the `detail` layer back to `.vox` via `Engine::exportVox`; exporting the full 500-voxel-wide arena exercises auto-chunking (>256 voxels/axis) and the lossy-property warning path

*Tests*
- [x] The five-layer arena config validates (integer ratios, modes, `decompose_to`) and a deliberately malformed variant is rejected (`LayerConfig::validate`)
- [x] Arena generators are deterministic (same layer + coord → identical grid) and the single-step `terraces`→`detail` decomposition is byte-for-byte stable (reuses the M6 determinism harness)
- [x] Headless unit tests for the game logic that does not need a window: key-collection state machine, win condition (all keys + goal reached), respawn trigger, and persistence round-trip of a player-built platform

- [x] **Demo — Arena platformer:** `07-arena-platformer` — spawn on the floor of a walled 500 m arena, collect the scattered `.vox` keys and reach the imported goal totem by traversing platforms that decompose from coarse blocks into fine 1 m detail as you approach; walk with gravity and collision across all five layers (G), build/break to make your own route (mouse, 1–9), toggle lava hazards (P), survey from free-fly (F), and export your modified arena to `.vox` (E)

---

### Phase 2 — 1.0 Release

> Phase 2 milestones are placeholders. Each will have a dedicated design task to produce a detailed plan before implementation begins. Order and scope are subject to revision after Phase 1 is complete.

**M8 — Material Property System** ✅

> M8 is the first Phase 2 milestone and the first to make a **simulation system read
> material properties and respond to their values** instead of to a block-type
> identity. The property struct (`MaterialProperties`) and its plugin registration
> path already exist and have since M1/M4 — every voxel carries `density`,
> `structural_strength`, `thermal_conductivity`, `porosity`, `hardness`, and
> `palette_index` by value, and five plugins register materials with real values.
> What is missing is any *consumer*: through M7b a voxel breaks instantly on a single
> click (`editVoxel(hit, Voxel::empty())`), ignoring `hardness` entirely, and the
> `src/simulation/` subsystem named in the project structure and ARCHITECTURE §5
> does not yet exist on disk. M8 creates that subsystem and uses **voxel-removal cost**
> as the worked example — the effort to remove a voxel is a pure function of its
> `hardness`, so a hard material and a soft one behave visibly differently with no
> block-type branching anywhere. Structural-strength collapse (M11) and
> thermal/porosity-driven flow (M12) are deliberately out of scope; M8 establishes
> the property-driven *pattern* those milestones follow.

*Design — the schema is fixed; the consumption contract is not*
- [x] Design task: confirmed `MaterialProperties` is complete for Phase 2 (no new field — per-material indestructibility uses a `hardness < 0` sentinel rather than a new bool) and wrote the consumption contract into ARCHITECTURE §5 — the voxel-removal-cost function (`hardness > 0` → work ∝ hardness, `time = hardness/power`; `hardness == 0` → instant, fail-soft default; `hardness < 0` → indestructible sentinel; `power <= 0` → never removes), and the general rule that a simulation system reads the voxel's own properties by value and never a material id (id lookup is tooling-only)

*Material property foundation (verified complete)*
- [x] Full material property struct on voxels — `MaterialProperties` (`include/plugin_api.h`) carries all six fields and is stored by value on `Voxel` (`src/world/Voxel.h`); copied into voxels at generation/import time (architecture §5)
- [x] Material definitions registered via plugin API — `register_material` records owner-tracked `RegisteredMaterial` entries in `PluginManager`; `base-terrain`, `water`, `hazards`, `arena`, and `layered-world` register materials with real property values; duplicate-id overwrite warns and per-plugin unload tears the entries down (M4 tests)

*Material registry lookup (cleanup — not on the simulation path)*
- [x] **(cleanup)** Add a queryable lookup to `PluginManager`: `material(material_id)` and `materialForPalette(uint8_t)` returning the registered `MaterialProperties` (or a documented neutral default), centralizing the keyed search that consumers currently hand-roll. Simulation reads properties off the voxel by value (architecture §5), so this serves tooling/import/UI, not the removal path
- [x] **(review old work)** Audit the existing registry consumers and move them onto the new lookup, accommodating any refinement it introduces: the hand-rolled 256-entry `palette_index → MaterialProperties` table in `VoxImporter::load` (`src/io/VoxImporter.cpp`), the positional `materials()[selectedMaterial]` build-selection in demos `04`/`07`, and the `find_if` scans in `PluginManagerTest` — confirming each behaves identically (or better) afterward

*Voxel-removal cost — the worked property-driven system*
- [x] `src/simulation/RemovalModel.{h,cpp}` (first file under `src/simulation/`): a pure, deterministic function mapping target `hardness` (and an optional tool `power`) to the work/time required to remove a voxel — `hardness > 0` → `time = hardness/power`; `hardness == 0` → instant; `hardness < 0` → never removable (indestructibility sentinel, no schema change); `power <= 0` → never removable. No block-type id, no plugin or renderer dependency, unit-testable in isolation
- [x] Per-target removal accumulator in the build/break tool path: holding the remove action on a voxel accrues progress scaled by `RemovalModel`; the voxel is cleared to `Voxel::empty()` (firing `on_voxel_modified` as today) only when accumulated work reaches the hardness-derived threshold, and progress resets when the targeted voxel changes. An indestructible (`hardness < 0`) target never accrues progress and is never cleared. This is transient tool state — not persisted, dirty tracking stays chunk-granular *(`sim::RemovalAccumulator`, `src/simulation/RemovalAccumulator.{h,cpp}`; wired into `04-build-break-persist` — hold left mouse to mine, harder voxels take longer; `tests/RemovalAccumulatorTest.cpp`)*
- [x] Removal-progress feedback through the existing highlight path: `BgfxRenderer::drawVoxelHighlight` reflects accumulated progress (crack stages / color ramp) plus a HUD readout of the target's `hardness`/`density`, so the player can see a harder material visibly take longer to clear *(`drawVoxelHighlight` gained an optional `progress` arg that ramps the outline toward red as `RemovalAccumulator::progress()` rises; `04-build-break-persist` feeds the active target's progress and shows hardness/density via `setHudText`. Renderer concern, kept in core and property-driven — plugins influence it through `register_material`, not a render hook)*
- [x] Example indestructible material: a plugin registers a `bedrock`-style material with sentinel `hardness < 0` and places it in the editable terminal layer; the removal tool refuses to clear it — the reference pattern for per-material indestructibility (distinct from whole-layer `VoxelMode::immutable`) *(`plugins/material-showcase/plugin.cpp` registers `bedrock` (`hardness = -1`) and lays it as the strata floor in the ordinary terminal layer; `RemovalAccumulator` never accrues against it, so it never clears — no block-type branch, the gate is the voxel's own `hardness`)*

*Tests*
- [x] `tests/RemovalModelTest.cpp`: removal effort is strictly increasing in `hardness` for `hardness > 0`; `hardness == 0` clears in one step; `hardness < 0` and `power <= 0` both report never-removable; identical inputs are deterministic; two voxels differing only in `hardness` require measurably different work (property-driven, not id-driven)
- [x] Material-registry lookup returns the registered properties for a known id/palette and the documented default for an unknown one, and stays correct across a plugin unload (`tests/MaterialRegistryTest.cpp` or an addition to `PluginManagerTest`) *(`PluginManager.MaterialLookupByIdAndPalette` in `tests/PluginManagerTest.cpp`: known id/palette → registered props, unknown id → neutral zero default, unknown palette → default carrying the requested index, and id/palette resolve back to the default after the owning plugin unloads)*

- [x] **Demo — Material matters:** `08-material-matters` — a flat strata world (built by the `material-showcase` plugin) of grass/dirt/stone/iron/diamond over an indestructible bedrock floor, each material differing in `hardness` (and `density`/`structural_strength`, all shown in the HUD); hold left mouse to mine — softer strata clear fast, harder ones take visibly longer (the highlight ramps toward red), and bedrock (`hardness < 0`) never clears; right mouse places the selected material (1-6). All effort is driven by the targeted voxel's own properties with no block-type branching on the removal path

**M9 — Composition Recipes and Feature Generators**

> M9 makes decomposition **recipe-driven**. Through M6/M7b a composite macro voxel
> decomposed by simply running its child layer's generator over the subvolume — a
> deliberate placeholder (`MacroVoxel.h`: "This holds no recipe yet (recipes are
> M9)"; `DecompositionJob` carries a bare `LayerGeneratorFn`). M9 replaces that with
> the **composition recipe** described in README §"Macro-Voxel Composition Recipes"
> and specified in ARCHITECTURE §6: a data record attached to a composite layer that
> drives child-grid generation through a weighted **material distribution**, an
> ordered list of **feature overlays** (caves, ore veins, …), **boundary overrides**
> for the macro voxel's faces, and **seed parameters** passed down to constrain child
> generation. This is the first milestone to **change `plugin_api.h`** — the
> deferred `register_recipe` hook lands, and `FeatureGeneratorFn` gains the parameter
> set + deterministic seed it has lacked since M4 (today feature generators take no
> parameters and are applied unconditionally to every chunk; M9 makes them
> recipe-referenced and parameterized). It is also the first to fire the **composite
> picking** path deferred in M6 (interacting with an undecomposed macro voxel runs
> its recipe). Scope boundaries: decomposition stays **single-step** (the deep N-layer
> cascade and cache-eviction policy are M10), and no structural/aggregate behavior is
> added (that is M11). Determinism is non-negotiable — the recipe is a pure function
> of `(world_seed, macro VoxelCoord)` per ARCHITECTURE §4, so evicted recipe-built
> chunks regenerate identically.

*Design — the recipe schema and the feature-generator contract*
- [x] Design task: pin down (a) the **recipe schema** — material-distribution spec (`(material_id, weight)` list + noise-function id + params), ordered `(feature_generator_id, params)` list, per-face boundary overrides, and `seed_parameters` key/value map; (b) the **recipe attachment model** — recipes registered per composite **layer name** (mirroring `register_layer_generator`), since the engine keys behavior by layer, not by an abstract "voxel type"; (c) the **extended feature-generator interface** — a parameter set plus a deterministic seeded RNG/seed handle, replacing the current parameterless `FeatureGeneratorFn`; (d) the **back-compat rule** — a composite layer that registers no explicit recipe gets a synthesized *default recipe* equivalent to the M6 "run the child generator over the subvolume" behavior, so demos `05`/`07` are byte-for-byte unchanged. Rewrite ARCHITECTURE §6 from aspirational to as-built and record the `plugin_api.h` additions

*Recipe schema and registry*
- [x] Flat `RecipeDesc` (public, `plugin_api.h`) + internal `Recipe` value type (`src/world/Recipe.h`): plugins build a POD `RecipeDesc` (pointers + counts — material-distribution spec, ordered feature-overlay list with per-entry params, top/bottom/side boundary overrides with per-face `depth`, and a `seed_parameters` array of tagged `RecipeParam`), so no std:: type crosses the plugin ABI; the engine deep-copies it into the owning `Recipe` (`std::string`/`std::vector`, no plugin/renderer/IO dependency) at registration, so the plugin's arrays need not outlive the call. Material/feature/noise ids stay as strings on `Recipe`; they resolve to `MaterialProperties`/`FeatureGeneratorFn`/`NoiseFn` only when a decomposition job is built on the main thread, so `DecompositionWorker` consumes a resolved job rather than `Recipe` or `PluginManager` directly *(`src/world/Recipe.h`: owning `Recipe` (+`DistributionValue`/`FeatureRefValue`/`BoundaryValue`/`RecipeParamValue`) with header-only `Recipe::fromDesc` deep-copying the flat `RecipeDesc`; null id/text pointers become empty strings; depends only on `plugin_api.h` + the standard library)*
- [x] `register_recipe(layer_name, const RecipeDesc*)` plugin hook in `plugin_api.h`, the deep-copied `Recipe` stored in an owner-tracked `RecipeRegistry` on `PluginManager` (lookup by composite layer name, neutral/default when unregistered), torn down on per-plugin unload exactly like the material/feature registries (the M4 ownership pattern) *(`PluginManager::recipes_` of `RegisteredRecipe{layer_name, Recipe, owner}`; the `register_recipe` lambda deep-copies and overwrites-by-layer-name with a warning; `findRecipe(name)` returns a `const Recipe*` (null ⇒ synthesized default at job-build); `eraseOwned` teardown added to all three unload/failed-init paths)*
- [x] Noise as a general engine facility (ARCHITECTURE §6): `NoiseFn` + `register_noise(noise_id, fn)` in `plugin_api.h`, an owner-tracked noise registry on `PluginManager` mirroring the built-in `.vox` handlers — `Engine::init()` registers a built-in set (`value`/`fbm`/`ridged`/`worley`, implemented in new `src/world/Noise.{h,cpp}`, the engine's first in-`src` noise) as built-ins; a plugin `register_noise` of the same id overrides a built-in (the importer dispatch rule) and plugin entries tear down on unload. The recipe distribution sampler is the first consumer; in-tree code calls `Noise.h` directly, while the plugin-consume accessor (`resolve_noise`) is deferred to M13 *(`src/world/Noise.{h,cpp}`: trilinear 3D value-noise base seeded by the threaded `uint64_t`, with `fbm`/`ridged` octave stacks and cellular `worley`, all returning `[0,1)`; `PluginManager::registerBuiltinNoise()` (called by `Engine::init`) installs the floor at `kBuiltinOwnerId`; `resolveNoise(id)` prefers a plugin entry over a built-in of the same id)*
- [x] Recipe validation folded into startup validation: every `composite` layer resolves to a recipe (explicit or synthesized default); a recipe that names a `feature_generator_id` or `noise_id` with no registered generator is a **startup error, not a silent skip** (ARCHITECTURE §6); material ids/palette entries referenced by a distribution resolve through the M8 `PluginManager::material`/`materialForPalette` lookup, falling back to the documented neutral default *(`validateRecipes(LayerConfig, PluginManager)` in `src/core/RecipeValidation.{h,cpp}`, run after plugins load: walks every `composite` layer, skips those with no explicit recipe (default), and throws `std::runtime_error` on the first feature/noise id that fails to resolve; material ids are intentionally not checked — fail-soft via the M8 lookup)*

*Recipe-driven decomposition*
- [ ] `DecompositionJob` carries a `Recipe` (and the resolved per-entry feature-generator fns + child material set) instead of a bare `LayerGeneratorFn`; the worker fills each child chunk by (1) sampling the material distribution into the grid, (2) applying boundary overrides on the macro voxel's exposed faces, then (3) running the recipe's feature overlays in declared order — one self-contained pure pass, still single-step (no cascade into further composites)
- [ ] Deterministic seeded RNG/seed derived from `(world_seed, macro VoxelCoord)` and threaded into the distribution sampler and every feature generator — the "seeded RNG provided by `DecompositionWorker`" promised in ARCHITECTURE §4/§14. No `rand`/`time`/unordered iteration is introduced; the existing M6 determinism guarantee is preserved
- [ ] The synthesized **default recipe** path keeps demos `05`/`07` running the child generator over the subvolume unchanged, proving the recipe layer is additive rather than a rewrite of the proven M6 decomposition flow
- [ ] **Composite picking** (the M6 deferral): a raycast that hits an undecomposed macro voxel in a composite layer enqueues its recipe-driven decomposition (via `DecompositionState::markPending`), so the player can trigger a recipe by interacting with the block, not only by approaching it

*Feature generators — now recipe-referenced and parameterized*
- [ ] Extend `FeatureGeneratorFn` / `register_feature_generator` with a parameter set and the deterministic seed handle (`plugin_api.h` change); migrate existing callers — the `water` plugin's sea-level generator and the hand-rolled `applyFeatureGenerators` loops in demos `03`/`07` — to the new signature, and route feature application through the recipe's ordered `(id, params)` list rather than "every registered generator over every chunk"
- [ ] Example **cave-network** feature generator plugin: carves connected void regions with seeded 3D value noise, parameters controlling threshold/scale; deterministic (no `rand`/`time`/unordered iteration). Plus an **ore-vein** feature generator that replaces material pockets with an ore material — the two overlays the demo recipe stacks
- [ ] Boundary overrides exercised by a real recipe: a surface-soil top face over a stone interior on the decomposed macro voxel, demonstrating per-face distribution distinct from the interior

*Hierarchical seed parameters*
- [ ] A parent composite recipe's `seed_parameters` are passed as generation inputs into the child layer's recipe/distribution at decomposition time, biasing the child grid (e.g. "bias toward granite", "no ore above depth N") — demonstrated **single-step** (parent composite → its immediate child) without the deep cascade reserved for M10; document the handoff point in ARCHITECTURE §6

*Tests*
- [ ] Recipe-driven decomposition determinism: the same `(recipe, world_seed, macro coord)` yields a byte-for-byte identical child grid across repeated and concurrent runs (extends the M6 `DecompositionTest` harness)
- [ ] Material distribution honors weights and is spatially stable for a fixed seed; boundary overrides land on the correct faces and only those faces; feature overlays run in declared order (an order-sensitive pair proves ordering)
- [ ] Cave generator carves the expected void fraction for given params and is deterministic; ore-vein generator replaces only the targeted material pockets
- [x] Noise registry: a built-in id resolves to the engine noise and is deterministic for a fixed `(pos, seed, params)`, a `register_noise` of the same id overrides it, and plugin noise entries are gone after the owning plugin unloads *(`tests/RecipeAndNoiseTest.cpp` `NoiseRegistry.*`: all four built-ins resolve, stay in `[0,1)` and are deterministic; the seed threads through; a plugin `register_noise("value")` overrides the built-in and the built-in floor is restored after unload)*
- [x] Recipe validation: a recipe naming an unregistered feature generator or `noise_id` is rejected at startup; a composite layer with no explicit recipe resolves to the default recipe; an unknown material id resolves to the neutral default; `RecipeRegistry` entries are gone after the owning plugin unloads *(`tests/RecipeAndNoiseTest.cpp` `RecipeValidation.*` + `RecipeRegistry.*`: unregistered feature/noise ids throw `std::runtime_error`; a recipe-less composite and an unknown material id both validate clean (material lookup falls back to the neutral default); `findRecipe` returns null after the owning plugin unloads, so validation passes again)*
- [ ] Seed-parameter passing: two distinct parent seed parameters produce measurably different — but each individually deterministic — child grids (proving the parent constrains the child, property-style, not by id branching)

- [ ] **Demo — Recipe-built voxel:** `09-recipe-built-voxel` — a composite layer whose registered recipe biases child materials, overrides the top boundary with surface soil, and stacks the cave-network + ore-vein feature overlays. Fly toward a macro voxel (or click it — composite picking) to decompose it and reveal a carved cave network shot through with ore veins under a soil cap; a key toggles the parent **seed parameter** between two values and re-decomposes, visibly changing cave density / ore richness while each value regenerates identically on revisit (determinism). Controls + run lines added to the Setup section when the demo lands

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

**M13 — Engine Generality (Beyond the Block Game)**

> Phase 1 proved the MVP, but several implementation choices made along the way
> quietly assume a single-scale, gravity-down, heightmap *block game* — and each
> one narrows the engine toward "Minecraft clone" even though the README promises a
> range from flying games to planetary and space simulation. The one we hit head-on:
> `LODManager`'s vertical streaming band is an **absolute** chunk-Y range, not a
> camera-relative volume, so a deep-dig world silently bottoms out on empty space
> the moment the dig leaves the configured band (the M8 material demo did exactly
> this). M13 is a deliberate generality pass: take the implicit Phase-1 assumptions
> and turn them into explicit, configurable engine *policy*, so the non-block-game
> configurations the architecture already describes actually work. No new
> simulation systems — this milestone widens what the existing ones are willing to
> assume about the world.

- [ ] Design task: audit Phase 1 for implicit single-scale / gravity-down / heightmap assumptions and produce an explicit **limitations catalog** with a generalization plan — streaming region-of-interest, vertical axis, gravity direction, and the terminal-as-primary forwarding in `World` are the known entries; the catalog is the deliverable that scopes the rest of this milestone
- [ ] Evaluate exposing the noise registry to out-of-tree plugins via a `resolve_noise(ctx, noise_id) → NoiseFn` accessor on `PluginContext` (the M9 deferral): M9 builds noise as a general facility — built-in set + `register_noise` override (ARCHITECTURE §6) — but only wires the decomposition sampler and in-tree consumers (which call `src/world/Noise.h` directly); a plugin's own layer/feature generators still hand-roll inline noise. Decide whether letting them resolve built-in/registered noise by id — and migrating `base-terrain`/`layered-world` off their inline value noise — belongs in this generality pass, per the §12 "add a function pointer to `PluginContext` when a consumer needs it" corollary
- [ ] **Configurable, camera-relative streaming region-of-interest, per layer:** replace `LODManager`'s fixed horizontal-square footprint + absolute vertical band with a pluggable streaming *volume* (box / sphere / shell) chosen per layer and centered on the camera, with no privileged vertical axis — so a deep-dig world streams downward as the player descends, a flying game streams a thin backdrop shell, and a space world streams a 3D box. This directly removes the vertical-band limitation that made the M8 dig demo end in empty space rather than on bedrock
- [ ] Decouple gravity direction and the "vertical" axis from streaming and collision: the engine is agnostic to which way is down — "down" can be absent (zero-g flight), a fixed alternate axis, or a direction that **varies per position and per frame** (radial wells around a planet, or the nearest body in a many-asteroid field) — so swept AABB resolution and the kinematic body work against an arbitrary, changing up-vector rather than a baked Y-down assumption
- [ ] Generalize the terminal-as-primary `World` forwarding so a world whose interactive layer is *not* the finest terminal layer (e.g. a flying game where the only editable layer is a mid-stack playspace) is a first-class configuration, not a special case
- [ ] Heterogeneous per-layer streaming budgets verified together: a tiny tight playspace volume and a vast sparse immutable backdrop shell streaming simultaneously within their own budgets, proving one stack can mix radically different scales and densities
- [ ] **Demo — Beyond blocks:** a deliberately non-Minecraft configuration on the same engine — e.g. a flying game whose only interactive layer is a small box playspace adrift inside a huge immutable backdrop, or a continuous vertical descent that streams with the camera all the way down — demonstrating that with the generalized streaming and axis policy the engine is genuinely multi-purpose, not a block-game with extra layers
- [ ] **Demo — Asteroid belt miner:** the complementary case to *Beyond blocks* — instead of one privileged "down", *many*. A rocketsuit player jets through a dense field of asteroids in zero ambient gravity; each asteroid is a composite voxel that decomposes on approach (M6) into a minable terminal grid, and exerts its **own radial gravity well** so the player can land on, walk around, and mine the surface of a body from any side — the same kinematic body and removal tool from M5/M8, but with "down" pointing at the nearest asteroid's center rather than a fixed axis. Streaming is a camera-centered 3D box with no vertical bias (asteroids surround the player in every direction), and mined-out resource voxels are driven by the M8 property system (richer ores are `hardness`-costlier). Together the two demos show two different ways to escape the block-game mold: *Beyond blocks* removes the gravity axis entirely, this one makes gravity **local and many-bodied**

**M14 — Polish and Release**
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

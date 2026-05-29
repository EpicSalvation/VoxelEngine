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
├── src
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
│   │   ├── Renderer.cpp / .h             # Layer-aware renderer, floating origin management
│   │   └── LODManager.cpp / .h           # Per-layer view distance and chunk budgets
│   ├── simulation
│   │   ├── PhysicsSystem.cpp / .h        # Material-property-driven structural simulation
│   │   └── PropagationSystem.cpp / .h    # Upward damage propagation across composite layers
│   ├── io
│   │   ├── VoxImporter.cpp / .h          # .vox format import with layer assignment
│   │   └── VoxExporter.cpp / .h          # .vox format export with auto-chunking
│   ├── plugins
│   │   └── ExamplePlugin.cpp / .h        # Reference plugin: feature generator + material def
│   └── main.cpp
├── include
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

```bash
git clone <repository-url>
cd voxel-game-engine
mkdir build && cd build
cmake ..
make
./voxel-game-engine
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
- [ ] Renderer reads a terminal-layer voxel grid and produces a visible window
- [x] Floating-origin pipeline in place (camera-local float submission to GPU)
- [ ] Palette-based material colors rendering correctly
- [ ] Basic free-camera movement for development/testing

**M3 — World and Chunk Management**
- [ ] Chunked terminal-layer world with configurable chunk size
- [ ] Chunk load/unload within a view distance budget
- [ ] Simple procedural generation for a single terminal layer (flat or heightmap)
- [ ] `LODManager` stub in place with per-layer view distance config read correctly

**M4 — Plugin System**
- [x] `PluginManager` loading and unloading plugins from disk
- [x] `plugin_api.h` stable for terminal-layer hooks: material registration, layer generator, voxel modification hook
- [x] Example plugin registers a material and a layer generator; documented as the canonical plugin reference
- [x] Plugin load errors reported clearly at startup

**M5 — Player Interaction**
- [ ] Player can place and remove terminal-layer voxels
- [ ] Dirty tracking at chunk granularity; modified chunks flagged correctly
- [ ] Basic save/load of dirty chunks to disk
- [ ] Collision detection against terminal-layer voxels

**M6 — Multi-Layer Support**
- [ ] Two-layer project config working (one composite layer above one terminal layer)
- [ ] Composite voxels render as solid atomic blocks before decomposition
- [ ] On-demand decomposition triggers correctly when player interacts with a composite voxel
- [ ] `DecompositionWorker` running on a thread pool with async pop-in; main thread never blocked
- [ ] Decomposition determinism verified by test: same seed produces identical child grid across multiple runs
- [ ] Immutable layer mode working: renders and collides, no decomposition, no persistence

**M7 — Voxel Editor Interoperability**
- [ ] `.vox` import assigns content to a specified layer at a specified world anchor
- [ ] `.vox` export with automatic chunking for volumes larger than 256³
- [ ] Lossy export fallback (extended data dropped, warning logged) working correctly
- [ ] Tested against a real MagicaVoxel-exported file

---

### Phase 2 — 1.0 Release

> Phase 2 milestones are placeholders. Each will have a dedicated design task to produce a detailed plan before implementation begins. Order and scope are subject to revision after Phase 1 is complete.

**M8 — Material Property System**
- [ ] Design task: finalize material property schema and physics integration contracts
- [x] Full material property struct on voxels
- [x] Material definitions registered via plugin API
- [ ] Simulation systems (mining resistance, etc.) driven by properties rather than IDs

**M9 — Composition Recipes and Feature Generators**
- [ ] Design task: recipe schema, feature generator interface, hierarchical seed parameter passing
- [ ] Recipe system implemented and referenced from composite voxel types
- [ ] Feature generator plugin hook live; example cave generator plugin written
- [ ] Hierarchical seed parameters passed from parent recipe to child recipe

**M10 — Cascading Multi-Layer Decomposition**
- [ ] Design task: cache eviction policy, memory budget across deep layer stacks
- [ ] Full N-layer decomposition chain working
- [ ] Clean chunk eviction and deterministic regeneration on cache miss verified by test

**M11 — Physics and Upward Damage Propagation**
- [ ] Design task: propagation algorithm, performance budget, immutable boundary behavior
- [ ] Structural strength aggregation from child voxels to parent composite
- [ ] Collapse events firing and cascading correctly
- [ ] Propagation confirmed to stop at immutable layer boundaries

**M12 — Fluid and Thermal Simulation**
- [ ] Design task: fluid and heat transfer algorithms, interaction with porosity and thermal conductivity properties
- [ ] Fluid flow driven by `porosity` material property
- [ ] Heat transfer driven by `thermal_conductivity` material property

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

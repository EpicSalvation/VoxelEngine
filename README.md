# Voxel Game Engine

## Overview

The Voxel Game Engine is a plugin-based C++ game engine designed for creating voxel-based games that go well beyond the conventional single-scale Minecraft model. Its defining architectural features are:

- **Hierarchical multi-layer scale system** — game makers define any number of voxel layers, each with its own base unit size, from centimeters to kilometers
- **Material-driven simulation** — voxels carry physical properties rather than hardcoded block type logic, enabling composable modding and emergent simulation
- **Lazy macro-voxel decomposition** — large-scale voxels stay atomic until player interaction demands sub-structure, then procedurally expand on demand
- **Standard tool interoperability** — compatible with `.vox` (MagicaVoxel) and `.qb` (Qubicle) for single-layer content; extended engine-native format for multi-layer and material features
- **Plugin architecture** — nearly all engine behavior including world generation, features, physics, and import/export is extensible via plugins

The engine is designed to support games ranging from a conventional Minecraft-style single-scale world to a multi-scale planetary simulation where players build meter-scale structures inside kilometer-scale terrain.

---

## Core Architectural Concepts

### Multi-Layer Scale System

Rather than fixing a single voxel size, the engine supports a user-defined stack of **layers**, each representing a different scale of the world. A game defines its layer stack in a project configuration file:

```yaml
layers:
  - name: "biome"
    voxel_size_m: 1000.0
  - name: "terrain"
    voxel_size_m: 10.0
  - name: "structure"
    voxel_size_m: 1.0
  - name: "detail"
    voxel_size_m: 0.1
```

**Validation rules enforced at engine startup:**

- At least one layer must be defined
- Each layer's voxel size must be strictly smaller than its parent
- The ratio of parent size to child size must be a whole integer (e.g. 10:1 or 100:1 — not 7.3:1)
- Minimum ratio between adjacent layers is 2:1
- Ratios implying a single-decomposition child grid exceeding the configured voxel budget will produce a warning with the actual child count printed explicitly

A simple Minecraft-style game defines a single layer and never interacts with any of this complexity. A planetary-scale game defines several.

**Why integer ratios only?** A parent voxel's interior must tile perfectly with child voxels. A 10m parent with 1m children produces exactly 10×10×10 = 1,000 child slots. A 7.3m parent produces a fractional remainder that cannot be represented cleanly in a grid. Non-integer ratios are rejected at load time with a clear error message.

### Coordinate Precision

World-space coordinates are stored and computed in **double precision** throughout the engine. Only the final GPU submission path converts to single-precision float, using a **floating origin** — the camera's world position becomes the local origin, and all scene geometry is translated into camera-local space before submission. This prevents the floating-point precision loss that silently corrupts sub-meter detail at kilometer scales.

This is a non-negotiable architectural constraint. Retrofitting double-precision coordinates after significant rendering code exists is extremely costly.

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

Mods that add new materials define property values rather than special-case code. Physics, fluid, and mining systems respond to properties, not IDs. This makes the simulation composable: a new volcanic rock mod doesn't require changes to lava flow logic.

For voxel-editor compatibility, the `palette_index` field maps to a standard 256-entry palette, allowing `.vox` import/export for the visual material layer even when extended properties are in use.

### Macro-Voxel Composition Recipes

A Layer 0 (or any macro-layer) voxel can carry a **composition recipe** that describes what it contains at the next layer down. Recipes specify:

- **Material distribution** — a weighted palette of child materials with a noise function controlling spatial arrangement (e.g. "80% granite, 15% quartz veins, 5% iron ore")
- **Feature overlays** — spatially-aware generators that stamp structures into the child grid (e.g. cave networks, water tables, ore clusters, dungeon seeds)
- **Boundary behavior** — how the top, bottom, and sides of the macro-voxel differ from its interior (surface soil, exposed rock faces, etc.)

Feature generators are registered by plugins. A recipe references feature generators by name. This means cave generation, dungeon placement, and any other sub-structure system are plugin-defined and composable.

### Lazy Decomposition

Macro-voxels stay **atomic** until something requires their interior. When a player interacts with (e.g. mines into) an undecomposed macro-voxel:

1. The engine checks whether a decomposed child grid already exists for that macro-voxel
2. If not, it runs the recipe procedurally to generate the child grid on a worker thread, with async pop-in to avoid hitching the main thread
3. The interaction proceeds against the now-real child voxels
4. The child grid is marked **dirty** (player-modified) and persisted; unmodified recipe-generated regions can be evicted and regenerated on demand

**Dirty tracking is at chunk granularity within a macro-voxel**, not per-voxel, to keep save file sizes tractable.

**Upward damage propagation:** Material loss in child voxels updates the macro-voxel's aggregate `density` and `structural_strength`. A heavily hollowed macro-voxel can become structurally unsound at the macro scale, triggering collapse — which may cascade to neighbors. This emergent behavior comes for free from the material property system.

Decomposition is **deterministic** — given the same recipe and world seed, the same child grid is always generated. This is required for eviction and regeneration to be transparent to players.

### Voxel Editor Interoperability

The engine is designed to remain compatible with standard voxel editors for the common case:

| Content type | Format | Compatibility |
|---|---|---|
| Single-layer, palette materials | `.vox`, `.qb` | Full import/export |
| Multi-layer or anchored content | Engine-native `.vxe` + `.vox` sidecar | Import/export via plugin |
| Extended material properties | `.vxe` | Engine-native only |

A plugin that adds non-standard features should also register an import/export handler. If none is registered, the engine falls back to vanilla `.vox` export (lossy but functional, with a logged warning).

The `.vox` format supports volumes up to 256³ per object. Larger volumes are automatically chunked on import/export.

---

## Project Structure

```
voxel-game-engine
├── src
│   ├── core
│   │   ├── Engine.cpp / .h          # Engine lifecycle, startup validation
│   │   ├── PluginManager.cpp / .h   # Plugin load/unload, hook registration
│   │   └── LayerConfig.cpp / .h     # Layer stack definition and validation
│   ├── world
│   │   ├── Voxel.cpp / .h           # Voxel data: material props, palette index, shape
│   │   ├── Layer.cpp / .h           # Per-layer chunk management and coordinate space
│   │   ├── World.cpp / .h           # Multi-layer world container
│   │   ├── MacroVoxel.cpp / .h      # Composition recipe, decomposition state
│   │   └── DecompositionWorker.cpp / .h  # Async on-demand child grid generation
│   ├── renderer
│   │   ├── Renderer.cpp / .h        # Layer-aware renderer, floating origin management
│   │   └── LODManager.cpp / .h      # Per-layer view distance and chunk budgets
│   ├── simulation
│   │   ├── PhysicsSystem.cpp / .h   # Material-property-driven structural simulation
│   │   └── PropagationSystem.cpp / .h  # Upward damage propagation across layers
│   ├── io
│   │   ├── VoxImporter.cpp / .h     # .vox format import
│   │   └── VoxExporter.cpp / .h     # .vox format export with chunking
│   ├── plugins
│   │   └── ExamplePlugin.cpp / .h
│   └── main.cpp
├── include
│   └── plugin_api.h                 # Public plugin interface
├── CMakeLists.txt
└── README.md
```

---

## Plugin API

Plugins can register handlers for the following engine hooks (subject to revision as the API matures):

- **World generation** — Layer N procedural population
- **Feature generators** — Sub-structure stamps used by composition recipes (caves, dungeons, ore veins, etc.)
- **Material definitions** — Register new materials with property values and palette entries
- **Import/Export handlers** — Custom format support or extended `.vox` sidecar data
- **Simulation hooks** — React to material state changes, structural events, or player interactions
- **Layer lifecycle hooks** — Called when a layer chunk is created, decomposed, modified, or evicted

To create a plugin, implement the interface defined in `include/plugin_api.h` and load it via `PluginManager`.

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

## Design Constraints and Known Hard Problems

These are documented here so contributors understand why certain architectural decisions exist and don't inadvertently work around them:

**Double-precision coordinates are mandatory.** Single-precision floats lose sub-meter accuracy at kilometer scales. All world-space math uses `double`. Only GPU submission uses `float`, via floating-origin translation. Do not change this without understanding the implications.

**Layer ratios must be integers.** The child grid of a macro-voxel must tile exactly. Non-integer ratios are rejected at startup, not silently rounded. If you encounter this error, fix the config — don't add a workaround in the decomposition code.

**Decomposition must be deterministic.** The same recipe + seed must always produce the same child grid. This is what allows unmodified child regions to be evicted and regenerated transparently. Do not introduce non-deterministic calls (e.g. `rand()`, `time()`, unordered map iteration) into the decomposition pipeline.

**Dirty tracking is chunk-granular, not voxel-granular.** Persisting every individual voxel modification would produce unmanageable save file sizes. The dirty flag marks whole chunks within a macro-voxel as player-modified. Chunk size is a tunable constant and should be set conservatively.

---

## License

MIT License. See LICENSE for details.

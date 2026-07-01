# Template Series

Well-commented boilerplate to jumpstart a new game on Lattice. Each
file is a complete, copy-paste starting point with the rationale inline — copy
it into place, rename, and start editing. They are grounded in the real engine
APIs (`include/plugin_api.h` and the engine headers) and mirror the patterns the
reference demos and plugins use.

These templates live under `templates/` and are **not** compiled by the build —
they are starting points, not built targets. Copy them into the locations below,
where the build's directory globs pick them up automatically (no CMake edits
needed).

## Index

| File | What it is | Copy it to |
|------|-----------|-----------|
| [`game/main.cpp`](game/main.cpp) | A runnable game entrypoint: engine + window + renderer bring-up, plugin loading, chunk streaming, fly-camera, and raycast break/place. | `demos/<NN-yourgame>/main.cpp` |
| [`game/world.yaml`](game/world.yaml) | An annotated layer config with three worked examples (single-layer, 3-layer Minecraft-like, immutable backdrop + playspace). | next to your `main.cpp`, or load inline |
| [`plugin/world-plugin.cpp`](plugin/world-plugin.cpp) | A world-generation plugin: materials, palette colours, a deterministic terrain generator, and an optional composition-recipe sketch. | `plugins/<yourname>/plugin.cpp` |
| [`plugin/gameplay-plugin.cpp`](plugin/gameplay-plugin.cpp) | A gameplay/hooks plugin: per-frame tick, voxel-modified reactions, material audio, and a structural-collapse response via `apply_edit`. | `plugins/<yourname>/plugin.cpp` |

## How a game fits together

1. **`world.yaml`** declares the *shape* of the world — how many scales (layers),
   voxel sizes, and streaming distances.
2. **A world plugin** (`world-plugin.cpp`) supplies the *content* — the materials
   and the procedural generator that fills each chunk. Generators must be **pure
   functions of (position, seed)** so streamed chunks regenerate identically.
3. **A gameplay plugin** (`gameplay-plugin.cpp`) supplies the *behavior* — it
   reacts to events the engine reports (edits, collapses, fluid/heat crossings)
   and pushes changes back through `apply_edit`.
4. **`main.cpp`** ties them together: it loads the plugins, streams chunks around
   the camera, renders them, and routes player input into voxel edits.

Plugins link **zero** engine symbols (ARCHITECTURE §12); they see only the flat
C-style API in `include/plugin_api.h` and receive a function-pointer table
(`PluginContext`). That is what makes them drop-in and hot-swappable.

## Wiring a plugin path into your game

The build injects each plugin artifact's absolute path as a preprocessor define
(e.g. `VOXEL_BASE_PLUGIN_PATH`). To load your own plugin, add a block mirroring
the existing ones in the root `CMakeLists.txt` (search for `VOXEL_BASE_PLUGIN_PATH`)
and reference that define from your `main.cpp`. The template `main.cpp` documents
exactly where.

## See also

- **Tutorials** — step-by-step walkthroughs of every feature these templates
  touch: [`docs/tutorials/`](../docs/tutorials/). Start with
  [01 — Hello Voxel](../docs/tutorials/01-hello-voxel.md) and
  [02 — Your First Plugin](../docs/tutorials/02-your-first-plugin.md).
- **Recipe book** — [`docs/creating-voxels.md`](../docs/creating-voxels.md) for
  materials and composition recipes.
- **Configuration** — [`docs/configuration-guide.md`](../docs/configuration-guide.md)
  for the YAML / `Tuning.h` / `EngineConfig` surfaces.
- **Reference implementations** — `demos/03-plugin-driven-world` and
  `demos/04-build-break-persist` (the patterns in `main.cpp`),
  `plugins/base-terrain` and `plugins/recipe-world` (the world plugin),
  `plugins/material-audio`, `plugins/crumble`, and `plugins/falling-debris`
  (the gameplay plugin).

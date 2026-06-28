# Tutorial 06: Importing Voxel Assets

Bring models from MagicaVoxel, Qubicle, or Blockbench into the engine. This
tutorial covers the import/export workflow for each supported format and the
round-trip editing cycle.

---

## Prerequisites

- Engine built and running ([Tutorial 01](01-hello-voxel.md))
- Comfortable writing a plugin ([Tutorial 02](02-your-first-plugin.md))
- Familiar with materials and palette colors ([Tutorial 03](03-materials-and-properties.md))

---

## 1. Supported formats

| Format | Extension | Editor | Scope |
|--------|-----------|--------|-------|
| MagicaVoxel | `.vox` | [MagicaVoxel](https://ephtracy.github.io/) | Single-layer, 256-entry RGBA palette, max 256x256x256 per object (auto-chunked for larger volumes) |
| Qubicle Binary | `.qb` | [Qubicle](https://www.minddesk.com/) | Single-layer, palette-color |
| Blockbench | `.bbmodel` | [Blockbench](https://www.blockbench.net/) | Textured blocks with per-face images |

All formats map to the engine's palette-indexed voxel model. Extended
`MaterialProperties` fields (density, hardness, structural_strength, etc.)
are **not** preserved in `.vox` or `.qb` round-trips -- only `palette_index`
survives.

### Choosing a tool

**MagicaVoxel** is the best starting point for most content. It is free, has a
mature sculpting/painting UI designed specifically for voxels, and its `.vox`
format is the engine's primary interchange format with full round-trip
import/export support. Choose MagicaVoxel when you are building solid-color
voxel models -- characters, props, terrain stamps, or anything where one
palette color per voxel is enough. Its 256-entry palette maps directly to the
engine's `palette_index`, so what you see in the editor is what you get
in-engine. The main limitation is the 256x256x256 size cap per object (the
engine auto-chunks larger volumes, but the editor itself enforces this bound).

**Qubicle** is a commercial alternative to MagicaVoxel with stronger
multi-matrix (multi-object) editing and a more traditional 3D-modelling
workflow. Choose Qubicle if you prefer its UI, need multi-matrix editing with
named parts, or already have a `.qb` asset library. The engine's `.qb` support
is functionally equivalent to `.vox` -- same palette-color scope, same
lossy-property caveat on round-trip.

**Blockbench** is the right choice when you need **per-face textures** rather
than a single solid color per voxel. It is free and open-source, and its
`.bbmodel` format embeds PNG textures that the engine's `blockbench` plugin
maps onto voxel faces via the texture atlas. Choose Blockbench for
Minecraft-style painted blocks where the top, bottom, and sides each show a
different image (e.g. a grass block with a green top, brown sides, and a dirt
bottom). The trade-off: `.bbmodel` import requires the `blockbench` plugin and
the M15 texture pipeline, so it is a heavier integration than plain `.vox`.

| Use case | Recommended tool |
|----------|-----------------|
| Solid-color models, props, characters | MagicaVoxel |
| Multi-object scenes, named parts | Qubicle |
| Per-face textured blocks (grass, crates, bricks) | Blockbench |
| Rapid prototyping (free, zero setup) | MagicaVoxel |
| Existing `.qb` asset library | Qubicle |

### Learning each editor

This tutorial covers getting assets *into and out of* the engine, not how to
sculpt voxels in the first place. For that, start with each tool's own learning
resources (developer-published where available):

- **MagicaVoxel** — The author (ephtracy) ships in-app reference rather than a
  step-by-step beginner course: see the **Shortcuts** and **Commands** pages
  linked from the [official site](https://ephtracy.github.io/). For a guided
  first model, the community
  [Introduction to MagicaVoxel](https://magicavoxel.fandom.com/wiki/Introduction_to_MagicaVoxel_0.99.1_(tutorial))
  on the MagicaVoxel Wiki and the
  [Mega Voxels beginner tutorial](https://www.megavoxels.com/learn/magicavoxel-tutorial-for-beginners/)
  are good entry points.
- **Qubicle** — Minddesk publishes official learning material: the
  [Qubicle documentation](https://getqubicle.com/qubicle/documentation/) and the
  [Learn Qubicle](https://getqubicle.com/qubicle/learn) hub (including a
  getting-started video) on the developer site
  ([minddesk.com](https://www.minddesk.com/)).
- **Blockbench** — Start with the official
  [Quickstart wizard](https://www.blockbench.net/quickstart/) to pick the right
  format, then the [Blockbench Wiki](https://www.blockbench.net/wiki/) for
  beginner modeling and texturing guides. Since the engine maps `.bbmodel`
  per-face textures, the wiki's texturing guides are the most relevant.

---

## 2. MagicaVoxel workflow

### 2.1 Create and export

Create a model in MagicaVoxel and save it as a `.vox` file. The file uses a
256-entry RGBA palette; each voxel references one index.

### 2.2 Import via Engine (high-level)

The simplest path uses the `Engine` class, which handles file I/O, palette
mapping, and chunk creation:

```cpp
engine.importVox("model.vox", "editor", WorldCoord(0, 0, 0));
// args: file path, target layer name, world-space anchor
```

The engine prefers a plugin-registered importer for `.vox` if one exists;
otherwise it falls back to the built-in `VoxImporter`.

### 2.3 Import via VoxImporter (standalone)

For direct control, use `VoxImporter` from `src/io/VoxImporter.h`:

```cpp
#include "io/VoxImporter.h"

VoxImporter importer;
importer.load("model.vox", *layer, WorldCoord(0, 0, 0), pluginManager);
```

Each placed voxel receives:
- `palette_index` set directly from the `.vox` color index
- Other `MaterialProperties` fields copied from the first `PluginManager`
  material whose `palette_index` matches; zero-defaults if none is registered

The file's authored RGBA colors are installed into the engine's shared visual
palette so imported voxels render with their original colors.

### 2.4 The VoxFile format

The low-level parse API in `src/io/VoxImporter.h`:

```cpp
namespace vox {

struct VoxFile {
    std::vector<VoxModel>      models;
    std::array<RgbaColor, 256> palette{};
};

// Parse raw .vox bytes. Returns false on bad magic / truncated data.
bool parse(const uint8_t* data, size_t size, VoxFile& out);

}  // namespace vox
```

Key constraints:
- Max 256x256x256 per object; larger volumes are auto-chunked into multiple
  objects with `nTRN` transform nodes encoding world offsets
- Palette entry 0 is unused (index 0 = empty in the `.vox` convention)
- When the file has no RGBA chunk, the built-in MagicaVoxel default palette is
  used

---

## 3. Export from the engine

### 3.0 Why export?

Importing pulls authored art *into* the engine; exporting sends voxels back
*out* to a standard file. Common reasons to reach for export:

- **Round-trip refinement.** Edit a model in-engine (or generate it
  procedurally), then export it back to `.vox`/`.qb` to polish in a dedicated
  editor where the sculpting/painting tools are richer. See section 6.
- **Capturing procedural or runtime content.** Terrain the engine generated, or
  a structure a player built at runtime, only exists as live voxels until you
  export it. Exporting turns that transient state into a reusable, hand-editable
  asset.
- **Building a shareable asset library.** `.vox` and `.qb` are widely understood
  interchange formats, so exported models drop straight into a team or community
  asset library and open in any compatible editor.
- **Authoring test fixtures.** Snapshot a known-good region to a file and check
  it into the repo as a regression asset for automated tests (see section 7 for
  building fixtures entirely in code).
- **External-pipeline interop.** Hand off to other voxel tools, converters, or
  renderers that read `.vox`/`.qb`.
- **Debugging and archival.** Dump a region's voxel state to inspect it in an
  external viewer, or archive authored content in a compact standard format
  under version control.

Keep the **lossy-property caveat** (section 6) in mind: only `palette_index`
survives a `.vox`/`.qb` round-trip; extended `MaterialProperties` are dropped.

### 3.1 Via Engine

```cpp
engine.exportVox("editor", minCorner, maxCorner, "output.vox");
```

The engine emits a `LOG_WARN` when any voxel in the region carries non-default
extended properties (density, hardness, etc.) because the `.vox` format silently
drops them.

### 3.2 Via VoxExporter (standalone)

```cpp
#include "io/VoxExporter.h"

VoxExporter exporter;
exporter.save("output.vox", *layer, WorldCoord(0, 0, 0), WorldCoord(4, 4, 4));
```

Regions larger than 256 voxels on any axis are auto-chunked: each 256-cubed
sub-volume becomes a separate SIZE+XYZI object with an `nTRN` transform node.

### 3.3 Computing the bounding box

To export all placed voxels, compute the bounding box from the layer's resident
chunks:

```cpp
int cs = layer->chunkSizeVoxels();
double vsz = layer->voxelSizeM();
int minX = INT_MAX, minY = INT_MAX, minZ = INT_MAX;
int maxX = INT_MIN, maxY = INT_MIN, maxZ = INT_MIN;

for (const auto& [cc, _] : editorLayer->chunks()) {
    minX = std::min(minX, static_cast<int>(cc.x));
    minY = std::min(minY, static_cast<int>(cc.y));
    minZ = std::min(minZ, static_cast<int>(cc.z));
    maxX = std::max(maxX, static_cast<int>(cc.x));
    maxY = std::max(maxY, static_cast<int>(cc.y));
    maxZ = std::max(maxZ, static_cast<int>(cc.z));
}

WorldCoord expMin(minX * cs * vsz, minY * cs * vsz, minZ * cs * vsz);
WorldCoord expMax((maxX + 1) * cs * vsz, (maxY + 1) * cs * vsz, (maxZ + 1) * cs * vsz);
```

---

## 4. Qubicle Binary (.qb) workflow

The `.qb` format follows the same pattern as `.vox`:

```cpp
// Import
engine.importQb("model.qb", "editor", WorldCoord(0, 0, 0));

// Export
engine.exportQb("editor", minCorner, maxCorner, "output.qb");
```

The same lossy-property warning applies: only `palette_index` survives the
round-trip.

---

## 5. Blockbench workflow

Blockbench models (`.bbmodel`) support per-face textures. Importing them
requires a plugin that uses the texture pipeline.

### 5.1 Register the importer in plugin init

```cpp
// In plugin init:
ctx->register_importer(ctx, "bbmodel", &importBlockbench, ctx);
```

### 5.2 Handle textures in the importer

A `.bbmodel` file embeds textures as base64 data URIs. The importer registers
them as in-memory image data:

```cpp
ctx->register_texture_data(ctx, texture_id, png_data, png_size);
```

Then bind the textures to material faces:

```cpp
ctx->set_material_faces(ctx, palette_index, top_tex, bottom_tex, side_tex, tiling_factor);
```

Where:
- `top_tex` is the texture_id for the +Y face
- `bottom_tex` is the texture_id for the -Y face
- `side_tex` is the texture_id shared by all four lateral faces
- `tiling_factor` is tiles per world meter (pass `1` for one image per face)

A null face string leaves that face unbound, rendering with the white tile
fallback. The binding keys on `palette_index`, not voxel size, so one texture
is scale-agnostic.

---

## 6. Round-trip editing

The typical workflow:

1. **Import** a `.vox` model into a terminal layer
2. **Edit** voxels in-engine (place, remove, modify)
3. **Export** back to `.vox` for further editing in MagicaVoxel

**Lossy-property warning:** extended `MaterialProperties` fields (density,
hardness, structural_strength, thermal_conductivity, porosity,
light_emission) are not preserved in `.vox` or `.qb` format. Only
`palette_index` survives the round-trip. If your workflow depends on these
properties, re-apply them via a plugin after re-importing.

---

## 7. Creating test assets procedurally

You can build and export test models entirely in code, without an external
editor:

```cpp
#include "core/LayerConfig.h"
#include "world/World.h"
#include "world/Layer.h"
#include "io/VoxExporter.h"

LayerConfig config = LayerConfig::loadFromString(R"(
layers:
  - name: editor
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 16
)");

World world(config);
Layer* layer = world.layer("editor");
layer->loadChunk({0, 0, 0}, nullptr);

// Place voxels...
Voxel v;
v.material.palette_index = 2;
v.material.density = 1.0f;
for (int x = 0; x < 4; ++x)
    for (int y = 0; y < 4; ++y)
        for (int z = 0; z < 4; ++z)
            layer->setVoxel(WorldCoord(x, y, z), v);

// Export
VoxExporter exporter;
exporter.save("test_model.vox", *layer, WorldCoord(0, 0, 0), WorldCoord(4, 4, 4));
```

This is useful for automated tests and procedural content pipelines.

---

## Challenge: prove the round-trip

Build an asset in code, round-trip it, and watch the lossy-property warning fire.

1. Start from the procedural example in section 7. Instead of a solid 4x4x4
   block, build a hollow box or a two-color checkerboard, and set a non-default
   `density` on the voxels.
2. Export it with `VoxExporter`, then import it back into a fresh layer with
   `VoxImporter`.
3. Confirm the geometry and palette survive -- and that the engine logs a
   `LOG_WARN` about dropped extended properties (only `palette_index`
   round-trips, per section 6).

<details>
<summary>Stuck? Where to look</summary>

- Start from the procedural snippet in section 7; APIs live in
  `src/io/VoxExporter.h` and `src/io/VoxImporter.h`.
- The dropped-property `LOG_WARN` is emitted by `exportVox` (section 3); the
  caveat is spelled out in section 6.
- Re-apply density with a `pluginManager.material("...")` lookup
  (Tutorial 03, section 5).

</details>

**Going further:** re-apply the density after import via a `PluginManager`
material lookup, restoring the property the `.vox` format dropped.

---

## How to verify

1. **MagicaVoxel round-trip:**

   ```bash
   cmake -B build && cmake --build build
   ./build/06-magicavoxel-round-trip
   ```

   The demo imports a `.vox` file, displays it in-engine, and lets you press
   **E** to export the result. Open the exported file in MagicaVoxel to
   confirm the round-trip preserved geometry and palette colors.

2. **Textured blocks:**

   ```bash
   ./build/15-textured-blocks
   ```

   This demo shows Blockbench-style textured blocks with per-face images
   rendered via the texture atlas pipeline.

---

## Key references

| What | Where |
|------|-------|
| VoxImporter (load API, VoxFile parse) | `src/io/VoxImporter.h` |
| VoxExporter (save API, auto-chunking) | `src/io/VoxExporter.h` |
| Engine import/export wrappers | [`include/core/Engine.h`](../../include/core/Engine.h) |
| Plugin importer/exporter registration | [`include/plugin_api.h`](../../include/plugin_api.h) (`register_importer`, `register_exporter`) |
| Texture registration (in-memory) | [`include/plugin_api.h`](../../include/plugin_api.h) (`register_texture_data`, `set_material_faces`) |
| MagicaVoxel round-trip demo | `demos/06-magicavoxel-round-trip/main.cpp` |
| Textured blocks demo | `demos/15-textured-blocks/main.cpp` |
| Blockbench plugin | `plugins/blockbench/plugin.cpp` |

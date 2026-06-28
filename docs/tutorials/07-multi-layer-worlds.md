# Tutorial 07: Multi-Layer Worlds

Design worlds with multiple voxel scales -- coarse terrain far away,
fine-grained detail up close. This tutorial covers layer stack planning,
validation rules, streaming configuration, and two worked examples.

---

## Prerequisites

- Engine built and running ([Tutorial 01](01-hello-voxel.md))
- Comfortable writing a plugin ([Tutorial 02](02-your-first-plugin.md))
- Understanding of composite vs. terminal modes ([Tutorial 04](04-composition-recipes.md))
- Familiar with boundary overrides ([Tutorial 05](05-multi-face-blocks.md))

---

## 1. Planning a layer stack

A multi-layer world is a stack of voxel grids at different scales. Each layer
has its own voxel size, streaming radius, and role:

- **Composite** layers are coarse macro voxels that lazily decompose into a
  finer child layer via a recipe. They provide the large-scale silhouette.
- **Terminal** layers are the editable leaf -- player-modifiable voxels at the
  finest scale.
- **Immutable** layers are render-and-collision only. They are never modified,
  decomposed, or persisted. Use them for static backdrops that never change.

The stack is defined in a YAML config and loaded via `LayerConfig::loadFromFile`
or `LayerConfig::loadFromString` (see
[`docs/configuration-guide.md`](../configuration-guide.md) for the full field
reference).

---

## 2. LayerDef fields

Each entry under `layers:` accepts the following fields, defined in
[`include/core/LayerConfig.h`](../../include/core/LayerConfig.h):

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `name` | string | (required) | Layer identifier; referenced by `decompose_to`. |
| `voxel_size_m` | double | (required) | Edge length of one voxel in meters. |
| `mode` | `composite` / `immutable` / `terminal` | (required) | Layer role (see above). |
| `decompose_to` | string | -- | Child layer name (composite only). |
| `chunk_size_voxels` | int | 32 | Voxels per chunk side (the chunk is this value cubed). |
| `view_distance_chunks` | int | 8 | Load/evict radius around the camera, in chunks. |
| `resident_chunk_budget` | int | 0 | Per-layer cap on resident chunks (0 = unlimited). The manager evicts farthest-first clean chunks to fit; near and dirty chunks are pinned. |
| `decompose_distance_m` | double | (unset) | Distance at which this layer's macro voxels decompose. Unset = falls back to the single radius passed to `DecompositionManager::tick()`. Setting it per layer **decouples the cascade**: reveal a coarse silhouette far out, build the fine grid only up close. |
| `streaming_shape` | `box` / `sphere` / `shell` | `box` | Shape of the camera-centered streaming volume. |
| `shell_thickness_chunks` | int | 1 | Band width for `shell` shape (inner radius = `view_distance_chunks - shell_thickness_chunks`). |
| `interactive` | bool | false | Marks the one layer the single-layer World API (get/setVoxel, dirty tracking, persistence, picking) targets. At most one layer may set this. |

The mode enum (`VoxelMode`): `composite`, `immutable`, `terminal`.
The shape enum (`StreamingShape`): `box`, `sphere`, `shell`.

---

## 3. Validation rules

The engine enforces these rules at config load time. An invalid config is a
hard error at startup -- the engine throws `std::runtime_error` with a
descriptive message.

1. **At least one layer** must be defined.
2. **`voxel_size_m` strictly descending** -- each layer must have a smaller
   voxel size than the one above it.
3. **Adjacent-layer size ratio is a whole integer >= 2** -- a parent voxel must
   tile cleanly into child voxels (e.g. 8.0 / 1.0 = 8 is fine; 8.0 / 3.0 is
   rejected).
4. **Every composite layer's `decompose_to`** must name a layer that exists in
   the config.
5. **At most one `interactive: true`** -- two or more is a hard error. When
   none is flagged, the engine defaults to the first terminal layer.

---

## 4. The `interactive` flag

In a multi-layer world, only one layer is the player's edit target. The
`interactive` flag explicitly declares which layer the World API
(`getVoxel`/`setVoxel`, dirty tracking, persistence, picking) forwards to.

Without the flag, the engine defaults to the first terminal layer. Set it
explicitly when your edit layer is mid-stack:

```yaml
layers:
  - name: blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    interactive: true    # player edits happen here
```

---

## 5. Per-layer streaming configuration

### view_distance_chunks and resident_chunk_budget

Each layer streams independently. `view_distance_chunks` controls how far from
the camera chunks are loaded; `resident_chunk_budget` caps memory by evicting
distant clean chunks.

For deep stacks, budget the coarsest layers tightly (they cover large areas with
few chunks) and give the finest layer a larger budget (many small chunks near the
player):

```yaml
  - name: blocks
    view_distance_chunks: 4
    resident_chunk_budget: 256
  - name: terrain
    view_distance_chunks: 6
    resident_chunk_budget: 2048
```

### decompose_distance_m

Decouples the cascade trigger from the streaming radius. Set a large value on
the coarsest composite to reveal its silhouette far out, and a small value on
finer composites to defer expensive decomposition until the player is close:

```yaml
  - name: macro
    voxel_size_m: 16.0
    mode: composite
    decompose_to: blocks
    decompose_distance_m: 200.0    # silhouette visible at 200 m
  - name: blocks
    voxel_size_m: 4.0
    mode: composite
    decompose_to: terrain
    decompose_distance_m: 60.0     # detail grid only within 60 m
```

### Streaming volume shapes

- **`box`** (default): isotropic 3D Chebyshev cube. Works for most block games
  with no vertical bias.
- **`sphere`**: Euclidean ball. Excludes the cube corners -- good for games
  where the player looks in all directions equally.
- **`shell`**: a thin Euclidean band resident only at range. Perfect for distant
  backdrops that never need interior detail -- the inner volume is empty, saving
  memory.

```yaml
streaming_volume:
  shape: shell
  shell_thickness_chunks: 2
```

---

## 6. Worked example 1: 3-layer Minecraft-like

A classic block game with three scales: macro terrain blocks at 8 m, a
mid-scale backdrop at 2 m, and the editable 1 m terrain.

```yaml
layers:
  - name: blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 8
    view_distance_chunks: 4
    resident_chunk_budget: 256
  - name: backdrop
    voxel_size_m: 2.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 6
    resident_chunk_budget: 512
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    view_distance_chunks: 6
    resident_chunk_budget: 2048
    interactive: true
```

The `blocks` layer provides the large-scale landform. As the player approaches,
macro voxels decompose into 8x8x8 grids of 1 m terminal voxels via the recipe
system. The `backdrop` layer is immutable scenery that never changes -- distant
mountains, ocean floor, sky islands.

### Wiring up in code

```cpp
LayerConfig layerConfig = LayerConfig::loadFromFile("world.yaml");

PluginManager pluginManager;
pluginManager.loadPluginsFromDirectory("plugins");

World world(layerConfig);
Engine engine;
engine.init(pluginManager, world);

BgfxRenderer renderer;
// ... initialize renderer ...
engine.setRenderer(&renderer);

constexpr uint64_t kWorldSeed = 42;
DecompositionManager decompMgr(world, pluginManager, layerConfig, kWorldSeed);
engine.setDecompositionManager(&decompMgr);
```

### Tick and render

```cpp
// Each frame:
auto diffs = decompMgr.tick(camPos, approachRadius,
                            loadPerFrame, decompPerFrame, applyPerFrame);
// Process diffs: build/destroy meshes for new/evicted chunks...

// Render coarsest first:
for (const auto& layerName : {"backdrop", "blocks", "terrain"}) {
    Layer* lyr = world.layer(layerName);
    for (const auto& [cc, mesh] : meshStores[layerName])
        renderer.renderChunk(mesh, lyr->getChunk(cc)->origin(),
                             lyr->voxelSizeM(), lyr->chunkSizeVoxels());
}
```

For worlds with large coarse layers, increase the far clip plane so distant
geometry is not culled:

```cpp
renderer.setFarClip(1200.0f);
```

---

## 7. Worked example 2: zero-gravity flying game

A space game with a distant immutable backdrop shell and a small playable island.

```yaml
layers:
  - name: backdrop
    voxel_size_m: 4.0
    mode: immutable
    chunk_size_voxels: 16
    view_distance_chunks: 10
    resident_chunk_budget: 512
    streaming_volume:
      shape: shell
      shell_thickness_chunks: 2
  - name: island
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 16
    view_distance_chunks: 6
    resident_chunk_budget: 1024
    streaming_volume:
      shape: box
    interactive: true
```

The `backdrop` uses a `shell` streaming volume -- only the outer band of chunks
is resident, forming a distant asteroid field or nebula. The `island` is a `box`
volume centered on the player, containing the mineable playspace.

The `sphere` shape for the backdrop would also work, but `shell` saves
significant memory by not loading interior chunks that the player never reaches.

### Layer access

```cpp
Layer* island = world.layer("island");
Layer* backdrop = world.layer("backdrop");
```

---

## 8. Multi-layer rendering

Render layers from coarsest to finest so that fine detail overlays coarse
geometry correctly:

```cpp
for (const auto& layerName : renderOrder) {
    Layer* lyr = world.layer(layerName);
    double vsm = lyr->voxelSizeM();
    int csvx = lyr->chunkSizeVoxels();
    for (const auto& [cc, mesh] : meshStores[layerName])
        renderer.renderChunk(mesh, lyr->getChunk(cc)->origin(), vsm, csvx);
}
```

The `renderChunk` method takes the layer's `voxelSizeM()` so each layer renders
at its correct world scale. Chunk meshes are built in chunk-local units (1 voxel
= 1 unit); the voxel size scales them to world space.

---

## Challenge: decouple the cascade

Take direct control of when silhouettes resolve into detail.

1. Start from the 3-layer Minecraft-like config (section 6) and add
   `decompose_distance_m` to the `blocks` layer (e.g. `48.0`) while leaving its
   `view_distance_chunks` large.
2. Load it in `05-decompose-on-approach` and fly toward a macro block: its
   silhouette should be visible far out but only resolve into fine voxels once
   you cross the decompose distance.
3. Switch the backdrop layer's `streaming_volume.shape` to `shell` and confirm
   only the outer band of chunks stays resident.

<details>
<summary>Stuck? Where to look</summary>

- Start from the config in section 6; demo
  `demos/05-decompose-on-approach/main.cpp`.
- `decompose_distance_m` and `streaming_volume.shape` are documented in
  sections 2 and 5 (full schema: `include/core/LayerConfig.h`).
- The validation rules you can trip are listed in section 3.

</details>

**Going further:** deliberately violate a validation rule (section 3) -- e.g.
give two adjacent layers a 3:1 voxel-size ratio -- and read the startup error.
The engine rejects invalid stacks loudly rather than misbehaving at runtime.

---

## How to verify

1. **Decompose on approach:**

   ```bash
   cmake -B build && cmake --build build
   ./build/05-decompose-on-approach
   ```

   Fly toward a composite block and watch it decompose into finer child voxels.
   Fly away and watch the child chunks evict (the composite block reappears as
   a single color).

2. **Beyond blocks:**

   ```bash
   ./build/16-beyond-blocks
   ```

   This demo shows a multi-layer world with streaming volume shapes and the
   shell backdrop pattern.

---

## Key references

| What | Where |
|------|-------|
| LayerDef, VoxelMode, StreamingShape | [`include/core/LayerConfig.h`](../../include/core/LayerConfig.h) |
| Layer config fields and validation | [`docs/configuration-guide.md`](../configuration-guide.md) section A |
| DecompositionManager (tick, cascade) | `src/world/DecompositionManager.h` |
| Engine setup for multi-layer | [`include/core/Engine.h`](../../include/core/Engine.h) |
| Decompose-on-approach demo | `demos/05-decompose-on-approach/main.cpp` |
| Beyond-blocks demo (shell, sphere) | `demos/16-beyond-blocks/main.cpp` |
| Architecture: multi-layer design | [`docs/architecture.md`](../architecture.md) |

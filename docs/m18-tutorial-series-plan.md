# M18 Tutorial Series — Plan

This document is the approved content plan for the M18 "tutorial series" task.
Each tutorial is a standalone Markdown file under `docs/tutorials/`, numbered
for progressive reading but usable independently. The series covers the
engine's full feature surface, from first build to performance tuning.

> **Existing doc note.** `docs/creating-voxels.md` already covers material
> registration and the grass-block recipe in depth. Tutorials 02–05 overlap
> with it deliberately — the tutorials are step-by-step walkthroughs aimed at
> newcomers, while `creating-voxels.md` is a concise recipe-book for someone
> who already knows the engine. Cross-link where appropriate; do not duplicate
> large code blocks verbatim.

---

## Tutorial index

| # | File | Title |
|---|------|-------|
| 01 | `01-hello-voxel.md` | Hello Voxel |
| 02 | `02-your-first-plugin.md` | Your First Plugin |
| 03 | `03-materials-and-properties.md` | Materials and Properties |
| 04 | `04-composition-recipes.md` | Composition Recipes |
| 05 | `05-multi-face-blocks.md` | Multi-Face Blocks via Recipes |
| 06 | `06-importing-voxel-assets.md` | Importing Voxel Assets |
| 07 | `07-multi-layer-worlds.md` | Multi-Layer Worlds |
| 08 | `08-camera-raycasting-interaction.md` | Camera, Raycasting, and Interaction |
| 09 | `09-player-mechanics.md` | Player Mechanics |
| 10 | `10-audio-and-sound.md` | Audio and Sound |
| 11 | `11-multiplayer.md` | Multiplayer |
| 12 | `12-simulation-fields.md` | Simulation Fields |
| 13 | `13-structural-collapse.md` | Structural Collapse |
| 14 | `14-performance-tuning.md` | Performance Tuning |

---

## Per-tutorial outlines

### 01 — Hello Voxel

**Audience:** Complete beginner to the engine.

**Covers:**

- Cloning, building (`cmake -B build && cmake --build build`), and running
  the `01-single-voxel` demo.
- Anatomy of a minimal demo `main.cpp`: creating an `Engine`, loading a
  `LayerConfig`, attaching a renderer, running the game loop.
- The YAML layer config format — what `voxel_size_m`, `mode`, and
  `chunk_size_voxels` mean.
- Fly-camera controls.
- Verifying the build with `ctest --test-dir build`.

**Outcome:** Reader can build the engine and create a bare-bones single-layer
world from scratch.

**Key references:** `demos/01-single-voxel/main.cpp`,
`include/core/LayerConfig.h`, `include/core/Engine.h`.

---

### 02 — Your First Plugin

**Audience:** Developer ready to extend the engine.

**Covers:**

- The plugin contract: a `plugin.cpp` under `plugins/<name>/` exporting
  `voxel_plugin_init` via `VOXEL_PLUGIN_EXPORT`.
- Drop-in build discovery (no CMake edits required).
- The `PluginContext` struct: callback registration via function pointers.
- Registering a custom material: `register_material`, `set_palette_color`.
- Writing a deterministic `LayerGeneratorFn` — the purity rule (no `rand()`,
  no `time()`, no global mutable state).
- Loading the plugin from a demo via `PluginManager`.
- The ABI version stamp: `VOXEL_PLUGIN_ABI_STAMP()` and what happens on
  mismatch.

**Outcome:** Reader has a working plugin that paints a flat colored terrain.

**Key references:** `include/plugin_api.h` (`PluginContext`,
`VOXEL_PLUGIN_ABI_VERSION`), `src/plugins/ExamplePlugin.cpp`,
`plugins/base-terrain/plugin.cpp`, `demos/03-plugin-driven-world/main.cpp`.

---

### 03 — Materials and Properties

**Audience:** Developer who wants simulation-reactive content.

**Covers:**

- The material-driven philosophy: voxels carry property bags, not block-type
  IDs. Simulation systems read properties; the renderer reads `palette_index`.
- Each `MaterialProperties` field and what engine system consumes it:
  - `density` → physics mass and structural load
  - `hardness` → mining time / removal resistance
  - `structural_strength` → collapse resistance (PropagationSystem)
  - `porosity` → fluid permeability (FluidSystem)
  - `thermal_conductivity` → heat spread (ThermalSystem)
  - `light_emission` → block light (LightingSystem)
  - `palette_index` → visual color (renderer palette)
- Palette colors: ABGR `0xAABBGGRR` format, translucency via alpha < `0xff`.
- Multi-material strata example (the `material-showcase` plugin pattern).
- How to verify: run `08-material-matters` and observe different mining times
  for different hardness values.

**Outcome:** Reader can define a full material set with meaningful physical
behavior.

**Key references:** `include/plugin_api.h` (`MaterialProperties`),
`plugins/material-showcase/plugin.cpp`, `src/renderer/Palette.h`,
`demos/08-material-matters/main.cpp`.

---

### 04 — Composition Recipes

**Audience:** Developer building procedural multi-scale worlds.

**Covers:**

- Composite vs. terminal vs. immutable modes.
- The decomposition chain: why macro voxels decompose one layer at a time
  (cascading lazy decomposition).
- Writing a `RecipeDesc`:
  - Interior `DistributionDesc`: `MaterialWeight` arrays, weighted selection.
  - Noise functions: built-in IDs (`value`, `fbm`, `ridged`, `worley`);
    registering custom noise via `register_noise`; consuming built-in noise
    from a plugin via `resolve_noise`.
  - `FeatureRef` overlays: cave networks, ore veins, dungeon seeds.
  - `seed_parameters`: biasing child-layer generation from the parent.
- Deterministic RNG: `voxel_rng_next`, `voxel_rng_norm`, `voxel_seed_mix`.
- Feature generators: the `FeatureGeneratorFn` signature, effective params
  (recipe entry params merged with inherited seed parameters).
- How to verify: run `09-recipe-built-voxel`, press T to toggle the
  `cave_density` seed parameter and watch the world re-decompose with
  visibly different cave density.

**Outcome:** Reader can author a multi-layer world with procedural recipes,
noise-driven material distributions, and feature overlays.

**Key references:** `include/plugin_api.h` (`RecipeDesc`, `DistributionDesc`,
`BoundaryDesc`, `FeatureRef`, `NoiseFn`), `plugins/recipe-world/plugin.cpp`,
`demos/09-recipe-built-voxel/main.cpp`.

---

### 05 — Multi-Face Blocks via Recipes

**Audience:** Developer who wants Minecraft-style visual variety from the
engine's recipe system.

**Covers:**

- The problem: a single terminal voxel renders as one solid color. How do
  you get green-top, brown-sides?
- The engine-native answer: a composite macro voxel with **boundary
  overrides** in its `RecipeDesc`.
- `BoundaryDesc` fields: `present`, `depth`, `distribution`.
- Overlap order at edges and corners: bottom → side → top (top wins the rim).
- Step-by-step walkthrough: building a grass block — green `top` override
  (depth 1), dirt `side` override, dirt interior.
- Going richer: multi-material interiors with noise, thicker caps,
  snow/ice/exposed-rock face overrides.
- Relationship to `docs/creating-voxels.md` (cross-link; that doc has the
  full recipe-book version).

**Outcome:** Reader can build visually interesting composite blocks using
boundary overrides without any engine changes.

**Key references:** `include/plugin_api.h` (`BoundaryDesc`, `RecipeDesc`),
`plugins/recipe-world/plugin.cpp`, `docs/creating-voxels.md` §4.

---

### 06 — Importing Voxel Assets

**Audience:** Artist or developer who creates content in external voxel
editors.

**Covers:**

- Supported formats and their scope:
  - `.vox` (MagicaVoxel): single-layer, palette-color, 256^3 max per object.
  - `.qb` (Qubicle): single-layer, palette-color.
  - `.bbmodel` (Blockbench): textured blocks with per-face images (M15).
- MagicaVoxel workflow:
  - Creating a model in MagicaVoxel; exporting as `.vox`.
  - Importing via `VoxImporter`: palette mapping, layer assignment, anchor
    position.
  - Auto-chunking for volumes exceeding 256^3.
- Blockbench workflow:
  - Creating a textured block model; exporting `.bbmodel`.
  - Importing via the `blockbench` plugin: `register_texture_data` for
    embedded textures, `set_material_faces` for per-face texture binding,
    `tiling_factor`.
- Round-trip editing: importing, modifying in-engine, exporting back.
  Lossy-property warning for extended material fields.
- How to verify: run `06-magicavoxel-round-trip` (import → edit → press E to
  export) and `15-textured-blocks`.

**Outcome:** Reader can create assets in MagicaVoxel or Blockbench and bring
them into the engine.

**Key references:** `src/io/VoxImporter.h`, `src/io/VoxExporter.h`,
`plugins/blockbench/plugin.cpp`, `demos/06-magicavoxel-round-trip/main.cpp`,
`demos/15-textured-blocks/main.cpp`.

---

### 07 — Multi-Layer Worlds

**Audience:** World designer building beyond single-scale.

**Covers:**

- Planning a layer stack: choosing scales, modes, and ratios.
- Validation rules enforced at startup: integer ratios ≥ 2:1, composite
  must name `decompose_to`, at most one `interactive` layer.
- The `interactive` flag: targeting the player's edit layer mid-stack.
- Per-layer streaming configuration:
  - `view_distance_chunks`, `resident_chunk_budget`.
  - `decompose_distance_m`: decoupling the cascade (silhouette far out,
    fine grid up close).
- Streaming volume shapes: `box`, `sphere`, `shell` — when each is
  appropriate.
- Worked example 1: a 3-layer Minecraft-like (macro terrain → blocks →
  detail).
- Worked example 2: a zero-gravity flying game with an immutable backdrop
  `shell` and a small `box` playspace (the `16-beyond-blocks` pattern).
- How to verify: run `05-decompose-on-approach` and `16-beyond-blocks`.

**Outcome:** Reader can design and configure a multi-layer world tailored to
their game concept.

**Key references:** `include/core/LayerConfig.h`,
`docs/configuration-guide.md` §A, `demos/05-decompose-on-approach/main.cpp`,
`demos/16-beyond-blocks/main.cpp`.

---

### 08 — Camera, Raycasting, and Interaction

**Audience:** Developer writing their first interaction code.

This is the bridge between "here is a world" (tutorials 01–07) and "here is
a playable game" (tutorials 09+). It introduces the three primitives every
interactive voxel game needs: camera control, picking, and world edits.

**Covers:**

- **Camera modes:** fly-camera (free movement) and walk-mode (the G-key
  toggle pattern used by existing demos). Setting camera position and
  rotation each frame via `Renderer::setCameraPosition` /
  `setCameraRotation`.
- **Raycasting / voxel picking:** the `voxelcast::raycast` API.
  - The `RayHit` struct: `hit`, `voxel` (remove target), `adjacent` (place
    target), `normal` (face direction), `distance`.
  - DDA grid traversal in double precision — why it uses `WorldCoord`, not
    float.
  - Building a ray from camera position + look direction.
- **Placing and removing voxels:** using `apply_edit` with the raycast
  result — `hit.voxel` for removal (pass `Voxel::empty()`), `hit.adjacent`
  for placement.
- **Voxel highlight:** `drawVoxelHighlight(center, size, abgr, progress)` —
  the wireframe targeting outline and the progress ramp that turns red
  during mining.
- **Mining laser visualization:** an exercise in combining the raycast with
  the per-frame `drawVoxel` API. Walk the ray from camera origin to
  `hit.distance` in small steps, calling `drawVoxel(stepPosition, laserColor)`
  at each interval. The result is a visible beam from the player to the
  targeted voxel, useful for mining-laser or tractor-beam effects. Covers:
  - Choosing step size and color.
  - Per-frame submission (the beam is transient — not placed in the world).
  - Performance note: `drawVoxel` is an immediate-mode submit, so a very
    long beam with tiny steps costs draw calls; recommend a step size of
    ~0.5–1.0 m.

**Outcome:** Reader can aim at voxels, break/place them, and render visual
feedback including a mining-laser beam.

**Key references:** `src/world/VoxelRaycast.h` (`voxelcast::raycast`,
`RayHit`), `include/renderer/Renderer.h` (`drawVoxel`,
`drawVoxelHighlight`), `include/plugin_api.h` (`apply_edit`),
`demos/04-build-break-persist/main.cpp`.

---

### 09 — Player Mechanics

**Audience:** Developer building a playable game with full first-person
controls.

**Covers:**

- The `kinematic-body` plugin: body registry, gravity, jump,
  sweep-and-resolve collision via `move_aabb`.
- The `register_on_tick` hook: per-frame simulation stepping.
- Input plugins: `keyboard-mouse` and `gamepad` — the device-agnostic
  input pattern (active device auto-switches when a controller is grabbed).
- Hardness-gated mining: progressive break with visual feedback (the
  highlight progress ramp from tutorial 08).
- The `on_voxel_modified` callback: reacting to block changes.
- HUD integration via the cell-grid overlay API (`hudClear`, `hudText`,
  `hudFill`, `hudCells`):
  - Health bar (hard landings deplete it).
  - Inventory hotbar (material slots, 1–5 / bumpers to select).
  - Top-down excavation minimap.
  - Status line: coordinates, active device, FPS.
- How to verify: run `18-hud-and-controls`.

**Outcome:** Reader can wire up first-person walk/mine/build gameplay with
a full game HUD.

**Key references:** `plugins/kinematic-body/kinematic_body.h`,
`plugins/kinematic-body/plugin.cpp`, `plugins/keyboard-mouse/plugin.cpp`,
`plugins/gamepad/plugin.cpp`, `src/renderer/BgfxRenderer.h` (HUD API),
`demos/18-hud-and-controls/main.cpp`.

---

### 10 — Audio and Sound

**Audience:** Developer adding audio to their game.

**Covers:**

- The audio subsystem architecture: `AudioManager`, positional audio,
  listener placement (`setListener(pos, forward, up)` each frame).
- Registering sound assets: `register_sound` with `SoundParams` (volume,
  attenuation model, min/max distance, rolloff, Doppler).
- Material-sound bindings: `register_material_sound` (palette_index +
  `AudioEvent` → sound_id). The engine resolves material → palette_index
  at registration, so play-time lookup is fast.
- One-shot positional sounds: `play_sound`, `play_material_sound`.
- Persistent positioned emitters: `create_emitter`, `set_emitter_position`,
  `stop_emitter`. Owner-tracked: plugin unload stops all its emitters.
- The `material-audio` plugin as a reference: break/place audio from
  `on_voxel_modified`, footsteps from the ground material.
- Asset path resolution: `assets/audio/` directory convention.
- How to verify: run `12-soundscape` — walk and build to hear
  material-appropriate positional sounds.

**Outcome:** Reader can add material-reactive positional audio to their game.

**Key references:** `include/plugin_api.h` (`SoundParams`, `EmitterParams`,
`register_sound`, `register_material_sound`, `play_sound`, `create_emitter`),
`plugins/material-audio/plugin.cpp`, `demos/12-soundscape/main.cpp`.

---

### 11 — Multiplayer

**Audience:** Developer building a shared-world experience.

**Covers:**

- The host-as-authority P2P model: `startHostPeer` (host) vs. `startClient`
  (client). Port and address configuration.
- Edit replication: how `apply_edit` flows through the authority and
  replicates to peers within one round-trip.
- Custom authority policy: `register_authority_policy` — accept/reject edit
  intents before they reach the authority.
- Edit conflict resolution: `register_on_edit_received`
  (Apply / Discard / Transform).
- Interest filtering: broadcast-all vs. streaming-radius vs. custom
  `register_interest_filter`. The trade-off between simplicity and bandwidth.
- Custom network messages: `send_network_message`,
  `register_on_network_message`, `MessageEnvelope`, channel routing.
- Player lifecycle: `register_on_player_joined`, `register_on_player_left`.
- **Player voxel representation:**
  - Tracking peer positions via `NetworkManager::playerPositions()`.
  - Rendering each remote player as a colored marker cube:
    `renderer.drawVoxel(peerPosition, peerColor)` each frame (the existing
    pattern from `11-shared-world`).
  - `drawVoxel` is per-frame / immediate-mode — the marker is transient
    (never placed in the world grid), so it moves with the peer and
    vanishes on disconnect with no cleanup.
  - Discussion: using multiple `drawVoxel` calls per peer to build a small
    multi-voxel "figure" (e.g. a 1×1×2 body + head) for more readable
    player markers. The same per-frame pattern, just more draw calls.
  - Contrast with persistent world voxels: `drawVoxel` is a rendering-only
    submit that does not modify the world or participate in collision /
    physics / persistence.
- The `chat` plugin as a worked example of custom channel messaging.
- How to verify: run `11-shared-world --host` in one terminal and
  `11-shared-world --join localhost` in another.

**Outcome:** Reader can add multiplayer to their game with edit replication,
custom messaging, and visible player markers.

**Key references:** `src/net/NetworkManager.h`, `include/plugin_api.h`
(networking types and hooks), `plugins/chat/plugin.cpp`,
`plugins/server-authority/plugin.cpp`,
`demos/11-shared-world/main.cpp` (lines 914–919: player marker rendering).

---

### 12 — Simulation Fields

**Audience:** Developer adding environmental simulation.

**Covers:**

- The field overlay model: sparse cellular automata riding atop the voxel
  grid. Fields are overlays — they do not add fields to `Voxel` or change
  the voxel format.
- **Fluid system:**
  - `register_fluid_source(pos, rate, fluid_material)`.
  - `on_fluid_event`: `FluidEvent` with `FieldCrossing::Rising` (saturated)
    / `Falling` (drained). The engine detects; the plugin responds via
    `apply_edit`.
  - Porosity-driven permeation: fluid seeps through porous materials,
    pools against impermeable walls.
- **Thermal system:**
  - `register_heat_source(pos, rate)`.
  - `on_thermal_event`: `ThermalFieldEvent` with Rising/Falling crossing.
  - Conductivity-driven diffusion: heat spreads faster through
    high-conductivity materials.
- **Lighting system:**
  - Sky light and block-emitter light (`light_emission` material field).
  - `register_light_source(pos, brightness)` for point lights.
  - `on_lighting_event`: `LightingEvent` with Rising/Falling crossing.
  - Ambient occlusion: vertex-factor AO baked into the mesh
    (`tuning::ao::kVertexFactor`).
- Per-frame budgets: tuning throughput vs. frame pacing via `EngineConfig`
  (`fluidMaxCellsPerFrame`, `thermalMaxCellsPerFrame`,
  `lightingMaxCellsPerFrame`).
- How to verify: run `14-flow-and-heat` (fluid + thermal) and
  `18-hud-and-controls` (lighting + AO in action).

**Outcome:** Reader can add fluid flow, heat propagation, and lighting to
their world.

**Key references:** `include/plugin_api.h` (fluid/thermal/lighting hooks and
source registration), `plugins/flow/plugin.cpp`,
`plugins/field-sources/plugin.cpp`, `docs/configuration-guide.md` §B
(thermal/fluid/lighting tuning constants),
`demos/14-flow-and-heat/main.cpp`.

---

### 13 — Structural Collapse

**Audience:** Developer wanting emergent structural behavior.

**Covers:**

- The propagation system: aggregate strength, support potential,
  support-flood. How child edits propagate upward to parent composites
  (density / structural_strength re-aggregation).
- Immutable boundaries: propagation stops at immutable layers.
- The detect/respond split (`docs/architecture.md` §7): the engine fires
  `on_structural_event`; a plugin decides the response.
- The `StructuralEvent` struct: position, layer, voxel size, aggregate
  strength, support potential, child voxel size.
- Reference response plugins:
  - `crumble`: clears the unstable macro's child voxels via `apply_edit`.
  - `falling-debris`: relocates material downward (gravity-aware).
- Multi-level collapse cascades: a collapse at one composite layer triggers
  re-aggregation in the grandparent, which may itself become unstable
  (referencing `19-multilevel-collapse`).
- Tuning: `kSupportSpanPerStrength`, `kMaxSupportSpan`,
  `kMaxStructuralEventsPerFrame`, `kMaxSupportFloodNodes`.
- How to verify: run `13-structural-collapse` and `19-multilevel-collapse`.

**Outcome:** Reader can configure structural physics and write custom
collapse-response plugins.

**Key references:** `include/plugin_api.h` (`StructuralEvent`,
`OnStructuralEventFn`, `apply_edit`), `src/simulation/PropagationSystem.h`,
`plugins/crumble/plugin.cpp`, `plugins/falling-debris/plugin.cpp`,
`demos/13-structural-collapse/main.cpp`,
`demos/19-multilevel-collapse/main.cpp`.

---

### 14 — Performance Tuning

**Audience:** Developer optimizing their game for release.

**Covers:**

- The three configuration surfaces:
  - **A. YAML** (`LayerConfig`): world shape — set once per project.
  - **B. `Tuning.h`** (compile-time `constexpr`): model constants — rebuild
    required.
  - **C. `EngineConfig`** (runtime): per-frame work budgets — live
    adjustment, quality presets.
- Per-frame work budgets: what each budget controls, how overflow carries
  to the next frame, how to expose quality presets to the player.
- Streaming tuning: chunk budgets, hysteresis margin, decompose-distance
  decoupling for cascaded layers.
- `EngineMetrics`: frame time, draw calls, resident chunks, decomposition
  queue depth, voice count. Replaces ad-hoc per-demo HUD stat computation.
- Logging: `Log::setMinLevel` (Debug / Info / Warn / Error), category
  tags, custom handlers.
- Fog and far-clip as LOD-hiding tools: `setFog(FogParams)`,
  `setFarClip(metres)`, `setClearColor` — dissolve the chunk-load edge
  into the background.
- Reference: `docs/m17-performance-profiling.md` for real profiling data.

**Outcome:** Reader can profile and tune their game's performance using all
three configuration surfaces.

**Key references:** `docs/configuration-guide.md`, `src/core/Tuning.h`,
`include/core/EngineConfig.h`, `include/core/Engine.h` (`getMetrics`),
`src/core/Logger.h`, `docs/m17-performance-profiling.md`.

---

## Writing guidelines

These apply to all tutorials in the series:

1. **Each tutorial is self-contained.** A reader who lands on tutorial 08
   should not need to have read 01–07 to follow along, though the series is
   designed for progressive reading. Prerequisites are listed at the top.
2. **Code snippets use real APIs.** Every snippet must compile against the
   actual `include/plugin_api.h` and engine headers. No pseudocode, no
   invented APIs.
3. **Every tutorial ends with "how to verify."** The reader runs a specific
   demo or test and observes a specific behavior. No tutorial is complete
   without a runnable proof.
4. **Cross-link, don't duplicate.** When a topic is covered in depth
   elsewhere (`docs/creating-voxels.md`, `docs/configuration-guide.md`,
   `docs/architecture.md`), link to it rather than repeating large blocks.
5. **Key references table at the end.** Each tutorial lists the source files
   the reader should have open while working through it.

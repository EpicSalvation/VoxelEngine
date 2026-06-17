# M15 Design Task — Textured Voxels & Content-Tool Interop Audit

**Status:** Design deliverable (scopes the rest of M15). No code is changed by this document.

**Scope note:** This audit covers the engine's **visual data model, rendering
pipeline, and import/export seam as they stand today**. It is the textured-voxel
counterpart to the M16 generality audit (`docs/m16-generality-audit.md`): where
that catalog widens the engine's *world-model* assumptions (axes, gravity,
streaming volume), this one closes the gap between a promise the README already
makes — **"Standard tool interoperability"**, envisioned as painting custom
voxels in tools like **Blockbench** — and a renderer that today draws **one solid
color per voxel**.

---

## 1. Purpose

The README's *Standard tool interoperability* bullet promises compatibility with
the well-known voxel/block authoring workflow: paint a block's faces in an
external editor (Blockbench being the canonical one for Minecraft-style textured
cubes), export, and have the engine render it. The current implementation honors
this **only for flat palette colors** — a `.vox` import installs a file's
256-entry RGBA palette and each voxel renders as a single shaded color
(`src/io/VoxImporter.cpp`, `src/renderer/Palette.h`). There is no path for a
**textured** voxel: no per-face image, no UVs, no texture atlas, no sampler in
the shader.

This catalog enumerates every implementation choice that assumes **a voxel is one
solid color**, rates it, and proposes a generalization toward **per-face textured
voxels authored in an external tool**. It is the deliverable that scopes the
remaining M15 work items.

Each entry uses the fixed shape shared with the M16 audit:

- **Where** — file/symbol and the load-bearing lines.
- **Assumes** — the implicit visual model.
- **Breaks** — the README-promised capability it makes impossible.
- **Generalize** — the proposed direction (design intent, not a final patch).

---

## 2. What is *already* in place (the seams to build on)

The engine is not hostile to textures; several seams already exist and are the
templates the rest should follow.

- **Material-driven design already reserves room for textures.**
  `MaterialProperties::palette_index` is documented as mapping to "color,
  texture, PBR params" (`README.md` §"Material-Driven Simulation",
  `include/plugin_api.h:28`). The data model *anticipates* this work — the
  property record is the natural key for a per-material texture lookup.
- **The palette is a runtime, per-index, plugin-installable table.**
  `palette::setColor` / `PluginContext::set_palette_color` already let a plugin or
  a `.vox` import bind appearance to a `palette_index`
  (`src/renderer/Palette.h:54`, `include/plugin_api.h:520`). A texture-atlas tile
  table keyed by the same index is the direct analog.
- **Import/export is already a pluggable, by-extension dispatch.**
  `register_importer(extension, ImporterFn)` / `register_exporter`
  (`include/plugin_api.h:590-602`) and the built-in `.vox` handlers
  (`src/io/VoxImporter.cpp`, `VoxExporter.cpp`) are exactly the seam a Blockbench
  importer plugs into — no new dispatch mechanism is needed, only a handler and a
  richer target representation (see T4).
- **The mesh builder already knows the six face directions.**
  `kFaces[6]` in `src/renderer/ChunkMeshData.cpp:21-38` carries each face's
  `(dx,dy,dz)` direction and a brightness multiplier. Per-face *texture tile*
  selection slots into the same table the per-face *shade* already uses.
- **The shader pipeline is authored + committed, not opaque.**
  `shaders/vs_voxel.sc`, `fs_voxel.sc`, `varying.def.sc`, and per-backend bytecode
  under `shaders/generated/{dxbc,essl,glsl,metal,spirv}` are in-tree and
  rebuildable (`architecture.md` §9), so adding a UV varying and a sampler is a
  contained, reviewable change.

The pattern: the *data-model intent* (textures keyed by material) and the
*extension seams* (palette table, importer dispatch, per-face table) are already
here. What is missing is the **rendering foundation** (UVs + sampler + atlas) and
a **richer import target** than a single `palette_index`.

---

## 3. The central design decision (decide before T1)

**Where does a face's texture live — on the *material*, or on the *voxel*?**

Two models are possible, and the choice gates the whole milestone:

- **(A) Material-keyed atlas (recommended).** A texture is a property of a
  *material/palette index*, not an individual voxel. The engine holds a texture
  **atlas** plus a table mapping `(palette_index, face_direction) → atlas tile`.
  The mesh builder emits UVs by looking up that table per face — exactly where it
  looks up `shade` today. A "grass block" is a material whose top face maps to a
  grass tile and whose sides map to a dirt tile.
  - **`Voxel` stays a trivially-copyable POD** — no new per-voxel field, so the
    memcmp determinism padding (`MaterialProperties::_pad[3]`), RLE chunk
    persistence (§9), and the plugin ABI are all **untouched**. This is the same
    discipline §6 invokes when it refuses to put a parent pointer on `Voxel`
    ("would break RLE persistence and the plugin ABI").
  - Limitation: every voxel of a given material looks identical (no per-instance
    unique texture). For the Blockbench/block-game workflow this is exactly the
    desired behavior.

- **(B) Per-voxel per-face data.** Each voxel carries its own face-texture
  references. Maximally flexible (unique decals per voxel) but it **enlarges the
  `Voxel`/material record**, which ripples into the determinism check, the §9
  chunk RLE format, `.vox` round-trip, and the plugin ABI — a schema change of the
  kind the architecture deliberately avoids.

**Recommendation: ship (A) in M15.** It delivers the full Blockbench textured-block
workflow with **zero `Voxel` schema change**, and leaves (B) as a clearly-scoped
future milestone if per-instance textures are ever required. Every limitation
below is written against (A).

---

## 4. Limitations Catalog (rendering tier)

### T1 — The vertex format carries no texture coordinates `[KNOWN]`

- **Where:** `src/renderer/ChunkMeshData.h:12-15` (`struct MeshVertex { float x,y,z;
  uint32_t abgr; }`) and the matching GPU layout `VoxelVertex::layout` in
  `src/renderer/BgfxRenderer.cpp:79-81` — `Position(3×Float)` + `Color0(4×Uint8)`
  only. No `TexCoord0`, no `Normal`.
- **Assumes:** a voxel face is fully described by a single packed color; geometry
  needs no surface parameterization.
- **Breaks:** any textured face — there is nowhere to put the UV that samples a
  texture across the quad. This is the foundational blocker; everything else
  depends on it.
- **Generalize:** add a `TexCoord0` (`2×Float` or normalized `Uint16`) — and a
  face/tile id if the atlas is indexed rather than UV-addressed — to both
  `MeshVertex` and `VoxelVertex::layout`, keeping them byte-compatible
  (`ChunkMesh.cpp` uploads `MeshVertex` memory through `VoxelVertex::layout`).
  Color stays (it modulates the texture and preserves the per-face shade).
- **Severity:** **High** — the spine of the milestone; T2/T5 consume it.

### T2 — The shaders do no texture sampling `[KNOWN]`

- **Where:** `shaders/vs_voxel.sc`, `shaders/fs_voxel.sc`, `shaders/varying.def.sc`,
  and the committed per-backend bytecode under `shaders/generated/`
  (`architecture.md` §9). The fragment shader outputs the interpolated vertex
  color; there is no `sampler2D` and no atlas bound.
- **Assumes:** fragment color = interpolated vertex color, full stop.
- **Breaks:** sampling a texture per fragment. Even with UVs on the vertex (T1),
  nothing reads them.
- **Generalize:** pass the UV through `varying.def.sc`, bind a texture-atlas
  sampler uniform, and have `fs_voxel.sc` sample it and modulate by the vertex
  color (so the existing per-face brightness and translucency still apply). Rebuild
  and commit the bytecode for all five backends — the §9 committed-shader workflow.
- **Severity:** **High** — paired with T1.

### T3 — There is no image/texture asset pipeline `[KNOWN]`

- **Where:** absent by omission. `assets/` contains **audio only**
  (`assets/audio/*.wav`); there is no image loader and no `bgfx::createTexture*`
  call anywhere in `src/renderer/`. The audio pipeline (M12) is the only asset
  ingest the engine has.
- **Assumes:** the engine never loads images; all appearance is procedural
  (palette colors).
- **Breaks:** loading a Blockbench-exported texture PNG and uploading it to the
  GPU as an atlas.
- **Generalize:** add a minimal texture-asset path — decode an image (a small
  header-only decoder or bgfx's bimg, consistent with the existing third-party
  vendoring), build/load a **texture atlas**, and upload via bgfx. Mirror the
  audio backend's ownership model: textures registered by a plugin are
  owner-tracked and torn down on unload (the §8 registry teardown contract).
- **Severity:** **High** — a genuinely new subsystem (the largest single piece).

### T4 — Voxel/material appearance is a single `palette_index` `[KNOWN]`

- **Where:** `include/plugin_api.h:22-32` (`MaterialProperties` — one
  `palette_index`, no texture or per-face fields) and `src/world/Voxel.h:7-16`
  (`Voxel` is `MaterialProperties` plus helpers; a trivially-copyable POD by
  design). `palette::color(idx)` is the sole appearance lookup
  (`ChunkMeshData.cpp:74`).
- **Assumes:** one material → one color, identical on all six faces (only the
  baked per-face *brightness* differs).
- **Breaks:** a material whose faces differ (grass top, dirt sides) — the literal
  grass-block case. There is no table the renderer can consult for "what tile does
  this material's top face use."
- **Generalize:** per design decision (A), add an engine-side
  `(palette_index, face) → atlas tile` table and a plugin entry point to populate
  it (e.g. `set_material_faces(ctx, palette_index, tile_top, tile_bottom,
  tile_side…)`), echoing `set_palette_color`. **No field is added to `Voxel` or
  `MaterialProperties`** — the index already on every voxel is the key, so the
  POD, the determinism padding, RLE persistence, and the ABI are all preserved.
- **Severity:** **High** — defines how content binds to geometry.

### T5 — The mesh builder applies flat per-face shade, not per-face tiles `[KNOWN]`

- **Where:** `src/renderer/ChunkMeshData.cpp:104-120` — the face emit loop pushes
  `MeshVertex{ position, faceColor }` with no UV; `kFaces[i].shade` is the only
  per-face variation.
- **Assumes:** per-face variation is a single brightness scalar.
- **Breaks:** assigning the four quad corners their atlas UVs and selecting the
  tile by face direction.
- **Generalize:** in the emit loop, look up the material's tile for `kFaces[i]`
  (T4 table), compute the four corner UVs into the atlas, and write them into the
  extended `MeshVertex` (T1). Keep `shade` as a color modulate so lit relief
  survives. The six-direction structure is already in `kFaces`.
- **Severity:** **Medium** — mechanical once T1/T4 land, but it is where they meet.

---

## 5. Limitations Catalog (interop / content tier)

### T6 — There is no Blockbench (or textured-format) importer `[M15 item]`

- **Where:** the built-in importers handle `.vox`/`.qb` palette content only
  (`src/io/VoxImporter.cpp`); `ImporterFn` (`include/plugin_api.h:352-359`) fills a
  `Voxel*` grid, and a `Voxel` can carry only a `palette_index` (T4) — so even a
  hand-written importer **cannot represent per-face textures today**.
- **Assumes:** imported content is palette-indexed voxels, nothing more.
- **Breaks:** importing a Blockbench model — its native `.bbmodel` (JSON: per-face
  texture refs + UVs + a referenced texture image) or its Minecraft block-model /
  OBJ+MTL exports.
- **Generalize:** once T4 gives a material→tile binding and T3 gives an atlas,
  add a Blockbench importer plugin that (a) ingests the texture image into the
  atlas, (b) registers materials whose face tiles point at the imported regions,
  and (c) fills the voxel grid with those materials. Register it through the
  existing `register_importer` seam. Provide an exporter too if `.bbmodel`
  round-trip is in scope (decision item — recommend a one-way import first).
- **Severity:** **Medium** — the user-facing deliverable, but it rests on T1–T5.

### T7 — The README "Standard tool interoperability" claim overstates textures `[doc/claim gap]`

- **Where:** `README.md` §"Standard tool interoperability" and the *Voxel Editor
  Interoperability* table list `.vox`/`.qb` as the interop story without noting it
  is **palette-color only** — no textured-tool (Blockbench) path exists yet.
- **Assumes:** a reader takes "standard tool interoperability" to include the
  textured-cube painting workflow this milestone is about.
- **Breaks:** nothing functionally, but it is the documentation oversight that
  prompted this milestone — the promise reads broader than the implementation.
- **Generalize:** add a short *not-yet-implemented* note to that section pointing
  at this milestone, and update both the section and the table to the textured
  reality once T1–T6 land.
- **Severity:** **Low** (documentation), but it is the entry point of the whole
  milestone and should be fixed in the same pass.

---

## 6. Severity & sequencing summary

| ID | Limitation | Tier | Severity | Enables |
|----|-----------|------|----------|---------|
| T1 | Vertex format has no texture coords | rendering | **High** | T2, T5 |
| T2 | Shaders do no texture sampling | rendering | **High** | the textured draw |
| T3 | No image/texture asset pipeline | rendering | **High** | T4, T6 (the atlas) |
| T4 | Appearance is a single `palette_index` | data model | **High** | T5, T6 |
| T5 | Mesh builder applies flat per-face shade only | rendering | Medium | textured faces |
| T6 | No Blockbench/textured importer | interop | Medium | the user workflow |
| T7 | README interop claim overstates textures | docs | Low | sets expectations |

**Recommended build order for the rest of M15:**

1. **T7 first (cheap, honest):** annotate the README interop claim as
   in-progress, so the promise matches reality while the work lands.
2. **T1 + T2 — the rendering foundation.** Extend `MeshVertex` /
   `VoxelVertex::layout` with UVs and teach the shaders to sample an atlas. Land
   them together behind a still-blank atlas so the pipeline is exercised before any
   content depends on it (default to a 1×1 white tile so existing colored worlds
   render byte-identically).
3. **T3 — the texture-atlas asset pipeline.** Image decode → atlas build → bgfx
   upload, owner-tracked per the §8 teardown contract.
4. **T4 + T5 — material→tile binding and per-face emission.** Add the
   `(palette_index, face) → tile` table and a `set_material_faces`-style entry
   point; emit per-face UVs in the mesh builder. **No `Voxel` schema change.**
5. **T6 — the Blockbench importer.** A plugin on the existing `register_importer`
   seam that ingests the texture + per-face mapping and registers textured
   materials. Exporter/round-trip is a follow-on decision item.
6. **Demo — Textured blocks.** A hand-authored Blockbench grass block (and a
   second multi-texture block) imported and rendered, proving the workflow
   end-to-end — the milestone's acceptance artifact, with the sample `.bbmodel` +
   texture committed under `assets/`.

**Out of scope (deliberately, candidates for later milestones):**

- **Per-voxel unique textures** (design decision (B)) — the `Voxel`-schema route;
  not needed for the block-authoring workflow.
- **PBR / normal / specular maps and animated textures** — appearance richness
  beyond a flat albedo atlas.
- **Greedy-meshing UV correctness** — if/when faces are merged across voxels, atlas
  UVs must tile per voxel; called out so it is not assumed solved here.

Consistent with the M16 audit's framing, this milestone **adds one new capability
(textured rendering + its asset pipeline)** rather than widening an existing
system's world assumptions — which is precisely why it is its own milestone and
sequenced ahead of the M16 generality pass.

---

## 7. Cross-check against the README's interoperability promise

The README's interoperability story names two threads; both are located and
scoped here:

- *Standard tool interoperability (palette content)* → **already works** via the
  built-in `.vox`/`.qb` palette importers (`src/io/VoxImporter.cpp`).
- *Standard tool interoperability (textured painting, e.g. Blockbench)* → **not yet
  implemented**; requires the rendering foundation (**T1/T2**), the asset pipeline
  (**T3**), the material→tile binding (**T4/T5**), and the importer (**T6**), with
  the claim itself corrected in the same pass (**T7**).

The data model already reserves the room (`palette_index` documented as mapping to
"color, texture, PBR"); this milestone fills it in.
</content>

# M16 Follow-up Audit — Generality Regressions Introduced by M15

**Status:** Design deliverable (a *second* generality audit, scoping additional M16
work items). No code is changed by this document.

**Scope note:** The first M16 audit (`docs/m16-generality-audit.md`) catalogued the
engine's single-scale / gravity-down / heightmap assumptions **as the tree stood
before M15**. The *Standard tool interoperability* milestone (**M15 — Textured
Voxels & Content-Tool Interop**) was then injected and **landed real code** (the
textured-voxel pipeline + Blockbench importer, commit `68b16f2` and its
predecessors). Because M15 shipped after the first audit, its changes were never
examined through the generality lens. This follow-up does exactly that: it re-reads
**only what M15 added or touched** and asks whether any of it re-entrenches a
privileged axis, a single scale, or a block-game assumption that the Engine
Generality push (M16) must now also widen.

Entries use the fixed shape shared with the two prior audits:

- **Where** — file/symbol and the load-bearing lines.
- **Assumes** — the implicit world/visual model.
- **Breaks** — the README-promised configuration it makes impossible or wrong.
- **Generalize** — the proposed direction (design intent, not a final patch).

---

## 1. What M15 touched (and what it did *not*)

M15's diff is confined to the **rendering, plugin-API, and import tiers**:
`ChunkMeshData`, `MaterialFaces`, `TextureAtlasData`/`TextureManager`,
`BgfxRenderer`, the shaders, `plugin_api.h`, `PluginManager`, the `blockbench`
plugin, and the demo/tests/assets. It did **not** touch `LODManager`, `World`,
`DecompositionManager`, `VoxelCollision`, `FluidSystem`, or `NetworkManager`.

**Consequence:** the first audit's catalog is intact. L1 (streaming box), L2
(collision grounding), L3 (fluid drain), L4 (`World` forwarding), L5 (immutable
residency), L6 (network interest), and L7 (no gravity seam) are **neither fixed nor
worsened** by M15 — none of their files changed. The build order in
`docs/m16-generality-audit.md` §5 still stands as written.

What M15 *did* do is introduce a **new appearance subsystem**, and that subsystem
brought a fresh privileged-axis assumption into a tier the first audit never had to
look at. That is the substance of this follow-up.

---

## 2. What M15 got *right* (the model to copy)

Worth recording, because it shows the regression below is a local slip, not a
milestone-wide one:

- **Texturing is scale-agnostic by construction.** The material→tile binding keys
  on `palette_index`, **not** voxel size, and the mesh builder scales the emitted
  UV span by `face_world_size × tiling_factor` (`ChunkMeshData.cpp:134`,
  `voxel_size_m * ft.tiling_factor`). The *same* authored tile therefore serves a
  1 m terminal voxel and a large composite block at a fixed world density — exactly
  the multi-scale discipline the engine wants. This is the textured analog of the
  "axis-free `neighbors6`" reference design and should be cited as a positive
  template, not flagged.
- **No `Voxel` schema change.** Decision (A) was honored: appearance stays a side
  table keyed by the existing index, so the POD, the memcmp determinism padding,
  RLE persistence, and the plugin ABI are untouched. No scale/precision foundation
  was disturbed.
- **The UV scale is isotropic.** The span uses one `voxel_size_m` for both in-plane
  axes (`uu`, `vv` at `ChunkMeshData.cpp:140-141`), so there is no X-vs-Z or
  per-axis stretch bias in the texturing math itself.

The regression is therefore **not** in the texture *format* or its scale handling —
it is purely in how a material names *which face* gets *which* tile.

---

## 3. Limitations Catalog (appearance / axis tier)

### G1 — The material-face binding hard-codes +Y as "up" (`top/bottom/side`) `[M15-introduced]`

- **Where:**
  - `set_material_faces(ctx, palette_index, top, bottom, side, tiling_factor)`
    (`include/plugin_api.h:807-814`), documented "`top` is the **+Y** face,
    `bottom` the **−Y** face, and `side` is shared by all four lateral faces."
  - `materialfaces::setMaterialFaces` and the `Face` enum
    (`src/renderer/MaterialFaces.h:33-55`): six faces collapse to **three**
    bindings — `PosY`(top), `NegY`(bottom), and one `side` reused for `±X` and
    `±Z`.
  - The Blockbench importer maps **`up`→top(+Y)**, **`down`→bottom(−Y)**, and any
    lateral (`north/south/east/west`) → the single `side`
    (`plugins/blockbench/plugin.cpp:316-334`).
- **Assumes:** "up" is always **+Y**, "down" is always **−Y**, and the four
  horizontal faces are visually interchangeable. This is the *same* gravity-down,
  Y-up block-game world model the first audit set out to dismantle — re-introduced,
  this time, into the appearance layer.
- **Breaks:** every configuration M16's L2/L7 work is meant to enable. The instant
  `gravityAt` (L7) returns anything but constant −Y, appearance and "up" diverge:
  - *Asteroid (radial gravity)* — a player walks on the **+X** or **−Z** face and
    expects the grass (the "up-facing", weathered surface) to face **away from the
    body's center**. The binding paints grass on **+Y** regardless, so a grass
    asteroid shows grass on one pole and dirt everywhere the player actually stands.
  - *Alternate-axis or zero-g world* — there is no engine-side way to say "the
    weathered face is the one opposing local gravity"; `top` is literally +Y.
  - *Four-lateral collapse* — even within a Y-up world this throws away the ability
    to give `±X` and `±Z` different tiles; more importantly it bakes in that "the
    four sides are equivalent," which is only true when up is ±Y. On a radial body
    the six faces are not partitionable into {up, down, 4×side} at all.
- **Generalize:** decouple the *authoring vocabulary* from the *world axis*. Two
  compatible directions, smallest first:
  1. **Keep six explicit faces in the binding** (bind per `kFaces` direction, not
     per top/bottom/side) so a material can at least differ on all six; make
     `top/bottom/side` a convenience that expands to the six. Cheap, no axis
     concept needed, removes the lateral-collapse half of the problem.
  2. **Resolve "up" against the gravity seam at mesh-build time.** Once L7 exists,
     the builder can pick which authored face (`up`/`down`/`side`) maps to which
     *geometric* face using the local `gravityAt` up-vector for that voxel/region,
     instead of the fixed `PosY/NegY` enum. The binding then stores a
     **gravity-relative** face role (`up | down | lateral`) and the renderer
     orients it — so a grass block is grass-side-out on any body. This is the
     appearance counterpart to L2's "grounded is blocked along the gravity vector"
     and should read its "down" from the **same L7 provider**, not a second one.
- **Severity:** **Medium–High.** It does not crash and is invisible in a Y-up
  world, but it makes both M16 demos (*Asteroid belt miner* especially) look wrong
  the moment they succeed mechanically — the textured surface and the walkable
  surface disagree. It is the appearance arm of L2/L7 and was missing from the
  first catalog only because M15 had not landed yet.

### G2 — The same `top/bottom/side` Y-privilege already lives in `RecipeDesc` (pre-existing; M15 copied it) `[first-audit miss]`

- **Where:** `RecipeDesc.BoundaryDesc top/bottom/side`
  (`include/plugin_api.h:445-462`): "`side` shared by all four lateral faces," with
  a fixed `bottom → side → top` overlap order. This is the M13 recipe/decomposition
  boundary-shell descriptor — and `set_material_faces`'s own doc cites it as "the
  **BoundaryDesc top/bottom/side convention**" (`plugin_api.h:794-795`). M15 did not
  invent the convention; it **adopted an existing one** the first audit never flagged.
- **Assumes:** a composite's decomposed boundary shells are partitioned into a top
  layer, a bottom layer, and four equivalent sides — i.e. +Y is up, for the
  *coarse* (atomic/composite) appearance as well as the fine one. Combined with G1,
  both the atomic-block texture LOD and its decomposed-shell distribution privilege
  +Y, so they stay consistent with each other — and consistently wrong off-axis.
- **Breaks:** an asteroid authored as a composite voxel (the *Asteroid belt miner*
  case) whose decomposed surface shell should be a **radial** crust, not a top
  slab. The boundary recipe can only put the "surface" distribution on +Y.
- **Generalize:** the same fix as G1, one tier deeper — let the boundary face roles
  resolve against the L7 up-vector rather than the fixed +Y/−Y enum, or at minimum
  expand to six explicit boundary faces. Note that this widens the first audit's
  scope: it is a genuine engine-tier axis assumption (M13), not appearance-only,
  and belongs in the L2/L3/L7 "axis-agnostic" cluster.
- **Severity:** **Medium.** Pre-existing, but it is the reason G1's fix should be a
  **shared** "gravity-relative face role" concept rather than a renderer-only patch
  — otherwise the fine textures and the coarse shells will be generalized
  separately and drift.

### G3 — The fixed per-face shade ramp bakes Y-up fake lighting, now modulating textures `[inherited / amplified]`

- **Where:** `kFaces[6].shade` (`src/renderer/ChunkMeshData.cpp:31-43`): "**top
  faces full brightness, sides dimmer, bottom darkest**." M15 keeps this as a color
  *modulate* over the sampled texel (`fs_voxel.sc`, and the
  `shadeColor(color, f.shade)` path at `ChunkMeshData.cpp:117-118`).
- **Assumes:** light comes from +Y; the +Y face is always brightest and −Y always
  darkest. Pre-existing (it predates M15), but **M15 amplifies it**: a correctly
  authored asteroid-surface texture is now additionally lit as though +Y is up, so
  even if G1 is fixed the relief shading still says "up is +Y."
- **Breaks:** consistent shading on any non-Y-up body — the lit relief contradicts
  the gravity-defined surface.
- **Generalize:** out of strict M15 scope (no M15 code introduced it), but flagged
  so the L7 work treats **shade direction** as another consumer of the gravity/up
  vector, not just grounding and fluid. A constant-−Y default keeps every current
  scene byte-identical. Low priority; record, don't necessarily fix in M16.
- **Severity:** **Low** (cosmetic; pre-existing; fold into L7's consumer list).

---

## 4. Non-issues checked and cleared

- **Texture atlas is globally resident, not streamed.** Analogous in shape to L5
  (immutable layers never stream), but textures are a bounded, content-sized
  resource shared across all chunks; making the atlas a streaming volume is neither
  required by any README configuration nor an axis/gravity assumption. **Not an M16
  item** — at most a future memory-budget concern for a world with thousands of
  distinct materials, which is an M17 polish question, not a generality one.
- **UV scale / tiling.** Isotropic and scale-agnostic — see §2. **Cleared.**
- **Vertex format / shader sampler / `Voxel` POD.** No world-model assumption; the
  format is direction-neutral. **Cleared.**
- **Importer dispatch (`register_importer`).** Extension-keyed and axis-neutral;
  the only axis assumption the importer carries is G1's `up→top` mapping, already
  catalogued. **Cleared otherwise.**

---

## 5. Severity & sequencing summary

| ID | Limitation | Tier | Severity | Relationship to first audit |
|----|-----------|------|----------|------------------------------|
| G1 | Material-face binding hard-codes +Y `top/bottom/side` | appearance | **Med–High** | New; the appearance arm of L2/L7 |
| G2 | `RecipeDesc.BoundaryDesc` has the same +Y privilege | engine (M13) | Medium | First-audit miss; engine-tier sibling of G1 |
| G3 | Fixed +Y-up shade ramp modulates textures | appearance | Low | Pre-existing; add to L7's consumer list |

**Recommended handling — fold into the existing M16 plan, do not re-sequence it:**

1. **Build L7 (the gravity provider seam) first, as already planned.** G1, G2, and
   G3 all want the *same* canonical up-vector L2 and L3 read from. Make "gravity-
   relative face role" a consumer of L7, alongside grounding (L2) and fluid (L3),
   so appearance and physics agree about "up" by construction.
2. **G1 + G2 together as one "axis-agnostic face roles" item.** Expand the binding
   (and `BoundaryDesc`) from `top/bottom/side` to either six explicit faces or a
   gravity-relative `up/down/lateral` role resolved at build time. Default
   resolution against constant −Y so every current world (and the M15 textured-
   blocks demo) renders **byte-identically** — the same regression-safety bar L1/L7
   already hold themselves to.
3. **G3 — record only.** Add "shade direction" to L7's list of consumers; fix is
   optional in M16, mandatory for a visually-correct off-axis demo.

**Net effect on M16 scope:** one new work item (G1+G2, the axis-agnostic face
roles, gated behind L7) and one documentation note (G3). No change to the L1–L7
spine or its build order. The *Asteroid belt miner* demo is the acceptance check —
it already exercises L2/L3/L7; G1/G2 make it also *look* right.

---

## 6. Cross-check against the first audit's framing

The first audit closed: "no new simulation systems — every item widens what an
existing system assumes." M15 honored the **simulation** half of that (it added a
rendering capability, not a simulation system) but, in doing so, **added a new
instance of the very world-model assumption M16 exists to remove** — and revealed a
pre-existing one (G2). This follow-up's single substantive conclusion: the Engine
Generality push must widen "up" in the **appearance** tier (G1/G2), not only in
collision (L2) and fluid (L3), and all three should read from the one L7 gravity
seam so they never disagree about which way is up.

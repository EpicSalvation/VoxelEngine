# M15 Design Task — Engine Generality Audit & Limitations Catalog

**Status:** Design deliverable (scopes the rest of M15). No code is changed by this document.

**Scope note:** The README frames this as a *Phase 1* audit. Per the milestone
owner's direction, this audit covers **the engine as it stands today**, regardless
of which milestone introduced a given assumption — so M13's collapse system, M14's
fluid/thermal systems, and the M10/M11 streaming and networking tiers are all in
scope, not just the M1–M9 core.

---

## 1. Purpose

The README promises an engine that spans "a conventional Minecraft-style
single-scale world all the way to a multi-scale planetary simulation … or a flying
game where the terrain is pure backdrop." Several concrete implementation choices
quietly assume a **single-scale, gravity-down (Y-up), heightmap block game** and
narrow the engine toward "Minecraft clone with extra layers."

This catalog enumerates every such assumption found in the current tree, rates it,
and proposes a generalization. It is the deliverable that scopes the remaining M15
work items.

Each entry uses a fixed shape:

- **Where** — file/symbol and the load-bearing lines.
- **Assumes** — the implicit world model.
- **Breaks** — the README-promised configuration it makes impossible or wrong.
- **Generalize** — the proposed direction (design intent, not a final patch).

---

## 2. What is *already* general (the models to copy)

The engine is not uniformly Y-biased; some subsystems already got this right and
are the templates the rest should follow.

- **`sim::neighbors6` / `NeighborWalk.h:30`** — the shared 6-connected
  neighbor walk is explicitly *"axis-free: no privileged down."* Used by the M13
  support flood and the M14 field passes.
- **`PropagationSystem` (M13 collapse)** — models *support reach* via an
  axis-free potential flood (`architecture.md` §7), not gravitational load.
  Its own comment notes "axis-free, so it does not bake in a 'down' direction —
  generalized gravity is M15's concern." This is the reference design for how a
  simulation can be scale- and direction-agnostic.
- **`WorldCoord` + floating origin** — already double-precision and
  camera-relative; the precision foundation for planetary/solar scale is in place
  and is *not* a limitation.
- **Noise (`src/world/Noise.h`)** — samples an arbitrary 3D world position; it has
  no surface/height bias of its own. The heightmap assumption lives in *callers*,
  not the facility.

The pattern is clear: where work was done *after* the multi-scale design was
internalized (M13/M14 detection), it is axis-free; where it was done early or in
content (collision grounding, fluid flow, terrain generators), it is Y-biased.

---

## 3. Limitations Catalog (engine tier)

### L1 — Streaming region-of-interest is a privileged-axis box `[KNOWN]`

- **Where:** `src/world/LODManager.{h,cpp}`.
  `desiredChunks()` (`LODManager.cpp:36-39`) emits a **Chebyshev XZ square**
  (`dx,dz ∈ [-r, r]`) crossed with a **separate Y band**
  (`setVerticalBand`, `LODManager.h:34`; defaults `±1,000,000`,
  `LODManager.h:65-66`). `withinViewDistance`/`shouldEvict` apply the radius to
  X and Z but treat Y specially.
- **Assumes:** the world is wide and shallow — a horizontal plane the camera
  travels across, with a small interesting Y range. The streaming volume shape is
  hard-coded (always an axis-aligned box) and **not selectable per layer**
  (`setVerticalBand` is a single global setter, not a `LayerDef` field).
- **Breaks:**
  - *Deep-dig world* — historically the band was absolute and a descent bottomed
    out in empty space (the M8 demo). The band is now intersected with
    `center.y ± r` (`LODManager.cpp:31-32`), so vertical tracking is partly fixed,
    **but Y remains a privileged axis** and the footprint is still an XZ disc, so
    a genuinely volumetric descent still streams a fat horizontal slab it does not
    need.
  - *Flying game / planetary backdrop* — wants a thin **shell** around the camera,
    not a solid box; the box wastes the entire budget on interior chunks.
  - *Space / asteroid field* — wants an **isotropic 3D box or sphere** with no
    vertical bias at all; the current XZ-square-×-Y-band is anisotropic.
- **Generalize:** replace the fixed footprint with a **pluggable per-layer
  streaming volume** (`box | sphere | shell`) centered on the camera with no
  privileged axis, selected via a `LayerDef` field. `desiredChunks` becomes a
  predicate over a `StreamingVolume` object. This is the spine of the milestone —
  L4, L5, and the network-interest entry (L6) all consume it.
- **Propagation:** the box shape is inherited downstream — `DecompositionManager`
  drives residency through `lod_.desiredChunks(camChunk, …)` /
  `lod_.shouldEvict(…)` (`DecompositionManager.cpp:542,558`), and the network
  interest filter mirrors the same `view_distance_chunks` (see L6). Fixing
  `LODManager` fixes all three.
- **Severity:** **High** — the single most-cited limitation; directly named in the
  milestone preamble.

### L2 — "Down" is hard-coded to −Y in collision grounding `[KNOWN: vertical axis]`

- **Where:** `src/world/VoxelCollision.cpp:78` — `if (axis == 1) result.grounded
  = true;` inside the per-axis snap. `MoveResult::grounded` is documented as
  "resting on a solid below (down-blocked)" (`VoxelCollision.h:22`).
- **Assumes:** the negative-Y face is "the ground." The swept-AABB resolution
  itself is **per-axis symmetric and already general** — only the *interpretation*
  of which axis is "down" is baked in.
- **Breaks:** any world where the support direction is not −Y — a walk-on-any-face
  asteroid (radial gravity), a fixed-alternate-axis world, or zero-g (no grounded
  concept at all). A player should be able to stand on the +X face of an asteroid
  and have `grounded` mean "blocked along the local down-vector."
- **Generalize:** make `moveAABB` (or its result) report **per-axis blocking**
  (already has `hitX/hitY/hitZ`) and have the caller derive `grounded` from "blocked
  along the current gravity direction." Better: pass an **up/gravity vector** into
  the kinematic step so grounding is computed against an arbitrary, possibly
  per-frame direction.
- **Severity:** **Medium** (small, localized change; large conceptual payoff).

### L3 — Fluid flow bakes in −Y gravity `[vertical axis / gravity]`

- **Where:** `src/simulation/FluidSystem.cpp:200-220`. Phase A is literally
  commented *"gravity drain. 'Down' is −Y (the engine's Y-up convention …)"* and
  drains into `{c.x, c.y-1, c.z}`; Phase B equalizes laterally over the **XZ**
  plane only (`kLateral = {±X, ±Z}`).
- **Assumes:** gravity points along −Y everywhere and forever. This is the
  *opposite* of the axis-free choice M13's collapse system made, in the very same
  `src/simulation/` tier.
- **Breaks:** fluid on a radial-gravity asteroid (should pool toward the body's
  center from any side), zero-g fluid (should not have a preferred direction at
  all), or any alternate-axis world. The asteroid-belt demo's "land and mine from
  any side" cannot have correct fluid.
- **Generalize:** parameterize the drain/equalize split by a **gravity direction**
  supplied to `FluidSystem::tick` (constant, or a field queried per cell). The
  6-neighbor structure is already there via `neighbors6`; the flow just needs to
  pick "downhill" relative to a vector instead of the fixed `y-1` neighbor. In
  zero-g the "drain" phase degenerates to pure pressure equalization across all 6
  neighbors.
- **Severity:** **Medium** — self-contained to one solver, but a real correctness
  bug for any non-Y-down world.

### L4 — `World`'s single-layer API forwards to the terminal layer only `[KNOWN]`

- **Where:** `src/world/World.cpp:26-29` selects `primary_` as the **first layer
  whose mode is `terminal`** (falling back to the first layer). The entire
  single-layer API — `getVoxel/setVoxel`, dirty tracking, persistence, the
  collision substep scale (`voxelSizeM`), picking — forwards to `primary_`
  (`World.cpp:64-103`).
- **Assumes:** the interactive layer is the finest terminal layer, and there is
  exactly one worth privileging.
- **Breaks:** a flying game whose only editable layer is a **mid-stack playspace**
  that is not the finest terminal layer; or a stack with more than one terminal
  layer (silently picks the first in config order). The README's flying-game
  configuration ("the only interactive layer is a mid-stack playspace") is exactly
  this case — currently a special case, not first-class.
- **Generalize:** make the primary/interactive layer an **explicit config choice**
  (a `LayerConfig`-level "interactive layer" selector or a per-layer
  `interactive: true` flag) instead of inferring it from mode + config order.
  Forwarding then targets the declared layer.
- **Severity:** **Medium**.

### L5 — Immutable layers are fully resident, never streamed `[single-scale]`

- **Where:** `architecture.md` §9 ("Immutable Layer Rendering … generated once at
  world load and retained"); the shipped immutable generators bear this out —
  `plugins/layered-world/plugin.cpp:172` and `plugins/recipe-world/plugin.cpp:190`
  generate the immutable slab "once and retained." Immutable layers do not
  participate in the `DecompositionManager`/`LODManager` residency cycle.
- **Assumes:** an immutable layer is small enough to hold entirely in memory
  (a bedrock floor, an arena wall).
- **Breaks:** the README's "vast sparse immutable backdrop shell" and the M15
  "thin backdrop shell streaming" item — a planet-scale or space backdrop cannot
  be fully resident. Heterogeneous budgets (L item below) require immutable layers
  to stream too.
- **Generalize:** bring immutable layers under the same per-layer streaming volume
  + budget as composite/terminal layers (they still skip dirty/persist, but their
  *meshes* stream in/out by residency). Cheap, because immutable chunks are
  regenerated-from-seed with no save path.
- **Severity:** **Medium**.

### L6 — Network interest mirrors the same axis-biased radius `[inherited]`

- **Where:** `src/net/NetworkManager.cpp:266` — `InterestMode::StreamingRadius`
  derives the interest region from `layerConfig_`'s `view_distance_chunks`, i.e.
  the same `LODManager` geometry as L1 (`architecture.md` §15 "mirror the local
  streaming radius").
- **Assumes:** whatever shape the streaming volume is, that is also the network
  interest shape. Today that inherits L1's box.
- **Breaks:** nothing *additional* — but it means the L1 fix must be expressed so
  the interest filter consumes the generalized volume rather than re-deriving a
  box. Listed so the generalization keeps the two in sync (and does not silently
  leave interest management Y-biased after L1 is fixed).
- **Generalize:** route interest through the same `StreamingVolume` predicate L1
  introduces.
- **Severity:** **Low** (follows automatically if L1 is done right; flagged to
  prevent drift).

### L7 — There is no engine concept of gravity at all `[gravity direction]`

- **Where:** absent by omission. Gravity is entirely **demo-side**: every walking
  demo hand-rolls `vy -= kGravity * dt` on the Y component
  (e.g. `demos/08-material-matters/main.cpp:60,288,297`). The engine ships the
  swept-AABB primitive (`moveAABB`) but no kinematic-body or gravity abstraction,
  so there is no seam where "down" could be made configurable, per-position, or
  per-frame.
- **Assumes:** each game re-implements Y-down gravity; the engine takes no
  position.
- **Breaks:** the M15 requirement that "down can vary per position and per frame"
  (radial wells, nearest-body-in-a-field). With no engine gravity seam, every
  game would have to re-derive radial gravity by hand, and L2/L3's "supply a
  gravity vector" generalizations would have nothing canonical to read from.
- **Generalize:** introduce a minimal **gravity provider** seam — a function
  `gravityAt(WorldCoord) -> dvec3` (constant −Y by default; a plugin or the game
  supplies radial/zero-g). The kinematic step, L2's grounding, and L3's fluid
  drain all read from it. Keep it a *provider/policy*, mirroring how authority and
  interest are policies (§15), not a baked engine force.
- **Severity:** **High** — it is the enabling abstraction for L2, L3, and both
  M15 demos; without it the "down is configurable" items have no home.

---

## 4. Limitations Catalog (content / plugin tier)

These are not engine limitations per se, but the **only shipped generators**
encode the block-game world model, so out-of-the-box the engine *looks* like a
heightmap engine. Worth cataloguing because the M15 demos need non-heightmap
generators and because the `resolve_noise` item unblocks them.

### C1 — All shipped world generators are Y-up heightmaps

- **Where:** `plugins/base-terrain/plugin.cpp:85-96` (a `terrainHeight(x,z)`
  column fill, Y up), `plugins/layered-world/plugin.cpp`. The
  coarse-occupancy-superset invariant (`architecture.md` §4) is itself explained
  in heightmap terms ("sample the coarse occupancy from the extreme of that field
  over the parent voxel's footprint").
- **Assumes:** a 2.5D heightmap surface with sky above and solid below.
- **Breaks:** asteroids (closed 3D bodies), floating playspaces, shells. Nothing
  in the engine forbids volumetric generators; none ship.
- **Generalize:** the M15 demos (asteroid belt, beyond-blocks) supply the first
  **volumetric** generators (radial density field), proving the engine hosts them.
  No engine change required — this is a demo/plugin deliverable.
- **Severity:** **Low** (content gap, not an engine constraint).

### C2 — Plugins cannot resolve built-in/registered noise by id `[M15 item]`

- **Where:** `architecture.md` §6 — `resolve_noise(ctx, noise_id) -> NoiseFn` on
  `PluginContext` is explicitly *deferred to M15*. Today out-of-tree plugins can
  only **provide** noise (`register_noise`) or hand-roll inline value noise
  (which `base-terrain`/`layered-world` do); they cannot **consume** the built-in
  registry.
- **Assumes:** a plugin's own layer/feature generators bring their own noise.
- **Breaks:** nothing functionally, but it forces every volumetric generator (C1)
  to re-implement noise, and contradicts the §6 "noise is a general facility"
  intent. The asteroid generator would want `fbm`/`worley` for surface relief.
- **Generalize:** add the `resolve_noise` function pointer to `PluginContext`
  (the §12 "add a function pointer when a consumer needs it" move) and migrate
  `base-terrain`/`layered-world` off inline noise. **Decision needed:** whether
  this belongs in M15 or is deferred again — recommend including it, since the new
  demos are the first real consumers.
- **Severity:** **Low–Medium** (enabler; cheap; decision item).

---

## 5. Severity & sequencing summary

| ID | Limitation | Tier | Severity | Enables |
|----|-----------|------|----------|---------|
| L1 | Streaming ROI is a privileged-axis box | engine | **High** | L4, L5, L6, both demos |
| L7 | No engine gravity seam | engine | **High** | L2, L3, asteroid demo |
| L2 | Collision grounding hard-codes −Y | engine | Medium | walk-any-face |
| L3 | Fluid flow bakes in −Y gravity | engine | Medium | fluid on asteroids |
| L4 | `World` forwards to terminal layer only | engine | Medium | flying-game playspace |
| L5 | Immutable layers never stream | engine | Medium | backdrop shells |
| L6 | Network interest inherits the box | engine | Low | keeps interest in sync |
| C1 | Only heightmap generators ship | content | Low | demos supply volumetric |
| C2 | No `resolve_noise` for plugins | content | Low–Med | volumetric generators |

**Recommended build order for the rest of M15:**

1. **L1 — pluggable per-layer streaming volume.** Foundational; unblocks L4/L5/L6
   and the streaming half of both demos. Express it so `DecompositionManager` and
   the network interest filter consume the same `StreamingVolume`.
2. **L7 — gravity provider seam** (`gravityAt(WorldCoord)`, default −Y). The home
   for all "down is configurable" work.
3. **L2 + L3** — make collision grounding and fluid drain read the gravity vector
   from L7. Now zero-g and radial gravity are correct end-to-end.
4. **L4** — explicit interactive-layer selection in config.
5. **L5** — stream immutable layers under the L1 volume/budget; then verify
   **heterogeneous per-layer budgets** (tiny playspace + vast sparse shell) — the
   M15 acceptance item.
6. **C2** — add `resolve_noise`; migrate in-tree generators (decision item).
7. **Demos** — *Beyond blocks* (zero gravity axis, shell/box streaming) and
   *Asteroid belt miner* (many-bodied radial gravity, isotropic box streaming,
   volumetric C1 generator) as the end-to-end proof.

**Out of scope (correctly):** no new simulation systems. Every item above widens
what an existing system assumes; none add a new one — consistent with the
milestone's "no new simulation systems" framing.

---

## 6. Cross-check against the milestone's known entries

The milestone preamble names four known entries; all are confirmed and located:

- *streaming region-of-interest* → **L1** (`LODManager`).
- *vertical axis* → **L2** (collision grounding) + **L3** (fluid drain).
- *gravity direction* → **L7** (no seam) + L2/L3 consumers.
- *terminal-as-primary forwarding in `World`* → **L4** (`World.cpp:26-29`).

Beyond those, the audit adds **L5** (immutable non-streaming), **L6** (network
interest inheritance), **C1** (heightmap-only generators), and **C2**
(`resolve_noise` enabler) as in-scope or decision items.

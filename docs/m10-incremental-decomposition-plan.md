# M10 Incremental Decomposition — Fix Plan

Status tracker for getting the M10 cascade ("drill to the core") decomposing
incrementally and performing well. Produced from a review of the engine core
(`DecompositionManager`, `DecompositionWorker`, `LODManager`, `Layer`, recipe
fill) and the M10 demo + drill-world plugin, on top of commit `1e06072`.

**The work spans multiple sessions.** Update the status checkboxes and the
session log at the bottom as items land, so any session can pick up where the
last one left off. Verify each item in the running demo before checking it off
— several fixes unmask the next one (see "Order of attack").

## Symptom being fixed

Only the chunk the player lands on decomposes and renders properly. Incremental
decomposition — approach a coarse block, it decomposes into finer blocks, which
decompose further on approach — is the core conceit of the system and must work
across the whole neighborhood of the player, then collapse cleanly when the
player leaves.

## Root cause summary

The headline bug is geometric: the approach trigger measures camera distance to
each macro voxel's **center**. With the demo's 64 m approach radius and 64 m
continental voxels, every neighbor of the block under the player has its center
68–80 m away even when the player stands on its edge — so exactly one block ever
decomposes. Fixing that single test makes the cascade spread, but immediately
surfaces four more P0 issues (re-atomization holes, a streamed-vs-recipe content
conflict, dead cave terrain, a GPU buffer leak) that were latent only because so
little decomposition was happening.

---

## P0 — Correctness (the incremental-decomposition conceit itself)

### 1. [x] Approach trigger: measure to voxel AABB, not center

- **Where:** `src/world/DecompositionManager.cpp` (`tick` step 3, distance check
  near lines 402–405).
- **Problem:** Sphere test `|macroCenter − camera| ≤ approachRadiusM`. A 64 m
  voxel's neighbors fail the test even at touching distance: walking on the
  surface (eye ≈ y 57), the block underfoot is ~24 m from its center but every
  horizontal neighbor is √(64² + 24²) ≈ 68 m away with a 64 m radius. This is
  the reported "only the landed chunk decomposes" symptom.
- **Fix:** Distance from camera to the closest point of the macro voxel's AABB
  (clamp the camera position into `[voxelOrigin, voxelOrigin + voxelSize]` per
  axis, then measure). Scale-free across layers; "within 64 m" then means
  within 64 m of the block's *surface*. Keep the `halfR` scan bound; it already
  covers the AABB-inflated radius (+1 voxel margin).
- **Verify:** From spawn, the 8 horizontal neighbors of the landing block (and
  diagonals whose faces are in range) decompose without flying into them; the
  cascade follows the player walking across the surface.

### 2. [x] Re-atomize parents when child chunks are evicted bottom-up

- **Where:** `src/world/DecompositionManager.cpp` (per-layer evict loop, lines
  ~330–361, and `enforceLayerBudget`); demo `applyDiffs` in
  `demos/10-drill-to-the-core/main.cpp`.
- **Problem:** Each composite layer's streaming evict runs independently.
  Regional's evict radius is (4+2)·64 m = 384 m; continental's is
  (3+2)·256 m = 1280 m. Flying ~400 m away evicts regional chunks that were
  *produced by continental decomposition*, but the continental macro stays
  marked decomposed with its block voxel cleared (cleared in `tick` step 1 via
  `setVoxel(empty)`). Result: in the 384–1280 m band, every previously
  decomposed continental block is an invisible, non-collidable,
  re-trigger-proof hole until the whole continental chunk evicts and reloads.
  The same happens at the local level in the 112–384 m band. The demo's "fly
  away and it collapses back to a coarse block" claim is currently false in
  those bands.
- **Fix:** When the manager evicts a child chunk that exists because of a
  parent's decomposition (the per-layer LOD evict loop and the budget pass), it
  must **re-atomize** the parent macro:
  - clear the parent layer's decomposed state for that macro;
  - restore the parent's block voxel — cache the original `Voxel` in
    `DecompositionState` at decompose time, or regenerate from the parent
    generator;
  - report the parent macro through `newlyAtomic` so the front-end remeshes the
    parent chunk.
  The demo must then actually handle `newlyAtomic` in `applyDiffs` (it
  currently ignores it, which is only valid for top-down eviction).
- **Verify:** Decompose an area, fly out to ~500 m, look back: coarse blocks
  visibly restored (no holes). Fly back in: blocks re-decompose identically
  (determinism check). Repeat for the local-layer band (~150–380 m).

### 3. [x] Single source of truth for child-layer content

- **Where:** `src/world/DecompositionManager.cpp` (`tick` step 2 streams every
  composite layer with its registered generator, lines ~303–326);
  `plugins/drill-world/plugin.cpp` (registers generators *and* recipes for all
  composite layers, lines ~208–215).
- **Problem:** Regional/local chunks come from two sources that disagree.
  Streaming runs each layer's generator (surface-following crust cells from
  `cellSolid`); decomposition inserts recipe-filled chunks (`fillChildChunk`
  fills the macro's *entire* volume solid — recipes have no occupancy/carve
  concept). `Layer::insertChunk` silently overwrites. Consequences:
  - z-fighting everywhere: undecomposed coarse blocks and streamed finer chunks
    have coplanar faces across the whole landscape within the finer layers'
    view distances;
  - decomposing a surface block *replaces* surface-following content with a
    full cube — visually nothing refines, only the color changes;
  - what is on screen depends on load/decompose history, violating the
    determinism story.
- **Fix (engine):** Stream only the **root** composite layer (the one with no
  composite parent) with a generator. Child composite layers receive chunks
  exclusively via decomposition (matches ARCHITECTURE §4 step 5: child voxels
  are created as atomic records by the parent's decomposition).
- **Fix (demo/plugin):** Make decomposition output match the shared surface
  field. Simplest coherent option: drop the plugin's `register_recipe` calls so
  the M6 generator-fallback path runs `regionalGen`/`localGen`/`terrainGen` per
  level. Each level then progressively refines the silhouette
  (64 m → 16 m → 4 m steps → carved 1 m terrain), which is exactly the
  incremental-decomposition visual this demo exists to show.
  *Option:* keep the recipe for continental→regional only (preserves a
  seed-cascade showcase) and accept the blockier middle step.
- **Verify:** No z-fighting shimmer on undecomposed surfaces. Decomposing a
  block visibly refines its silhouette rather than just recoloring it.
  Decompose → evict → re-approach yields identical content.

### 4. [x] Cave-carved 1 m terrain actually generates

- **Where:** `src/world/DecompositionManager.cpp` `makeJob` (~line 83) prefers
  a recipe when registered; `plugins/drill-world/plugin.cpp` registers
  `kLocalRecipe` for "local" (~lines 182–190, 215).
- **Problem:** local→terrain decomposition recipe-fills solid 4³ cubes with a
  soil cap; `terrainGen` (caves, surface contour, hollow core) is dead code.
  The demo header advertises terrain "carved by caves" that cannot appear.
- **Fix:** Remove the "local" recipe so the terminal step uses `terrainGen`
  (falls out of item 3 if all recipes are dropped). **Longer-term engine item
  (P2):** recipes need an occupancy/carve mechanism before they can drive
  surface terrain.
- **Verify:** Decomposed 1 m terrain shows surface contour, soil cap, and cave
  openings; digging down reaches the hollow core.

### 5. [x] Fix GPU buffer leak on remesh; dedupe remeshes

- **Where:** demo `applyDiffs`, `demos/10-drill-to-the-core/main.cpp` lines
  ~76, ~91, ~98.
- **Problem:** `compMeshes[cc] = ChunkMesh::build(*chunk)` overwrites an
  existing `ChunkMesh` without `destroy()` (the `remeshTerrainChunk` lambda
  does it correctly). The line-91 path fires on **every decomposition**. bgfx
  caps static vertex/index buffers at 4096 each (ARCHITECTURE §10 notes) and
  the overflow failure mode is silent invisible-but-solid chunks. Once item 1
  lands and decomposition runs at scale, this leak hits the cap quickly.
- **Fix:** Destroy-before-reassign in all three `applyDiffs` paths; dedupe
  `newlyDecomposed` entries by owning composite chunk so one chunk isn't
  rebuilt up to 64× in a tick.
- **Verify:** Long session of continuous flying/decomposing shows no
  invisible-but-solid chunks; (optionally instrument handle counts).

---

## P1 — Performance / hitching

### 6. [x] Cheapen and bound the approach scan

- **Where:** `DecompositionManager.cpp` `tick` step 3 (lines ~373–418).
- **Problem:** Brute-force O((2r/voxelSize)³) sweep per layer per tick — for
  the local layer (4 m voxels, 64 m radius) that is 35³ ≈ 43k iterations, each
  doing a hash lookup (`needsDecompose`) *before* the cheap distance test, plus
  a `getVoxel` (two more hash lookups). Gets hotter once item 1 lands.
- **Fix:** (a) Reorder gates cheapest-first: distance → `needsDecompose` →
  parent-decomposed → solid. (b) Iterate the layer's *resident chunks*
  intersecting the radius instead of every voxel in the cube (most of the cube
  is air or unloaded). (c) Optional frontier model: only re-examine macros when
  their parent decomposes or the camera crosses a voxel boundary.

### 7. [x] Budget the drain / spread mesh builds

- **Where:** `DecompositionManager.cpp` `tick` step 1 (lines ~275–297); demo
  `applyDiffs`.
- **Problem:** All completed worker jobs apply in one tick and the demo builds
  every resulting mesh on the main thread in the same frame — the documented
  >0.5 s hitches (see the dt-clamp comment in `main.cpp` ~lines 251–254).
- **Fix:** Apply at most N drained results per tick (keep the rest queued in
  the manager); dedupe composite-chunk remeshes per tick. Deeper lift (later):
  move `ChunkMesh::build` data generation off the main thread.

### 8. [x] Nearest-first job ordering

- **Where:** `DecompositionManager.cpp` `tick` step 3 + FIFO queue in
  `DecompositionWorker`.
- **Problem:** Jobs enqueue in scan order (from the −halfR corner), so with a
  tight per-frame budget the block under the player can queue behind dozens of
  peripheral ones.
- **Fix:** Collect candidates, sort by camera distance, then enqueue (or make
  the worker queue a priority queue keyed at enqueue time). Directly improves
  perceived decomposition latency.

### 9. [x] Per-chunk pending counters instead of voxel scans

- **Where:** `DecompositionManager.cpp` evict pin checks (lines ~337–344) and
  `enforceLayerBudget` (~224–229).
- **Problem:** Both eviction paths scan all 64 voxels of every candidate chunk
  per tick to test `isPending`.
- **Fix:** Maintain a ChunkCoord → pending-count map in `DecompositionState`
  (increment on `markPending`, decrement on `markDecomposed`/`clear`).

### 10. [x] Don't let the manager's own writes pin chunks as dirty

- **Where:** `DecompositionManager.cpp` drain step `setVoxel(empty)`
  (~lines 294–296); `enforceLayerBudget` dirty pin (~line 221).
- **Problem:** Clearing the decomposed block via `Layer::setVoxel` marks the
  composite chunk dirty, and the budget pass pins dirty chunks — so any
  composite chunk that ever hosted a decomposition becomes budget-unevictable.
- **Fix:** Distinguish render-state writes from player edits (a clean-write
  path on `Layer`, or clear the dirty flag after the manager's own write).
  Coordinate with item 2's voxel-restore mechanism.

### 11. [x] Configure budgets and vertical band; skip empty meshes

- **Where:** demo layer config (`main.cpp` ~lines 122–150);
  `LODManager::desiredChunks` / `setVerticalBand`.
- **Problem:** No `resident_chunk_budget` set on any layer (ARCHITECTURE §11
  expects per-layer caps, especially fine layers, to stay under the bgfx handle
  ceiling). `desiredChunks` loads a full cube including sky and hollow-core
  chunks (continental: 7³ = 343 chunks for a world occupying one chunk-Y band),
  and the demo's mesh stores accumulate empty `ChunkMesh` entries iterated
  every frame.
- **Fix:** Set per-layer `resident_chunk_budget`; call `setVerticalBand` to the
  slab extent (note: the band is currently global to the LODManager, may need
  to be per-layer); skip mesh-store entries for empty chunks. Depends on item 2
  (budget eviction of decomposition-fed layers needs re-atomization).

---

## P2 — Polish / follow-ups

### 12. [x] Update stale drill-world plugin header comment

`plugins/drill-world/plugin.cpp` lines 1–11 still describe the old
512 m / ratio-8 stack; generators now hardcode the shallow 64/16/4 sizes.
Will likely be rewritten as part of item 3.

### 13. [x] Cache recipe-resolution work in `makeJob`

`DecompositionManager.cpp` `makeJob` re-resolves the recipe and re-walks the
ancestor chain per macro on the main thread. Cache the ancestor index walk and
per-layer merged base params (only `__altitude`/`__parent_material` vary per
macro). Moot for the demo if item 3 drops recipes, still worth doing for
recipe-driven worlds.

### 14. [x] Frustum culling in the demo render loop

`main.cpp` ~lines 424–434 draws every resident mesh unconditionally. Fine at
current scale; revisit before view distances grow.

### 15. [ ] Engine design item: recipe occupancy/carving

Recipes fill a solid macro's entire child volume; they cannot express partial
occupancy (surfaces, caves). Until they can, recipe-driven stacks can only
produce full-cube refinement. Design a carve mechanism (e.g., an occupancy
noise/threshold on the recipe, or a generator-composed mask) before using
recipes for surface terrain.

---

## Order of attack

1. **Items 1 + 5** (small, surgical): incremental decomposition visibly works
   without exhausting the bgfx handle pool. *Test checkpoint: walk/fly around,
   cascade spreads, no invisible chunks.*
2. **Items 3 + 4** (mostly deleting plugin recipes + stopping
   generator-streaming of non-root composite layers): refinement actually looks
   like refinement; caves appear. *Test checkpoint: no z-fighting, silhouette
   refines per level, caves present.*
3. **Item 2** (largest engine change): collapse-on-departure works.
   *Test checkpoint: fly out/back at multiple ranges, no holes, deterministic
   regeneration.*
4. **Items 7 + 8** first among P1 (they govern perceived latency), then 6, 9,
   10, 11.
5. **P2** as opportunity allows.

## Session log

| Date | Session | Items touched | Notes |
|------|---------|---------------|-------|
| 2026-06-12 | review | — | Initial review and this plan. No code changes yet. |
| 2026-06-12 | fixes-1 | 1, 5 | AABB approach distance in `DecompositionManager::tick`; destroy-before-rebuild + per-chunk remesh dedupe in demo `applyDiffs`. New regression test `CascadeDecompositionTest.ApproachTriggerUsesVoxelSurfaceDistance` (verified failing on the old code). Full suite green. **In-demo verification still pending** (this environment is headless): walk/fly around, confirm cascade spreads to neighbors and no invisible chunks after long sessions. |
| 2026-06-12 | fixes-2 | 3, 4, 12 | Engine: only root composite layers are generator-streamed (`tick` step 2 gated on `parentIdx < 0`); non-root composite chunks now come exclusively from parent decomposition. Plugin: dropped all three recipes so every cascade step uses the M6 generator path (`regionalGen`/`localGen`/`terrainGen`) — caves now reachable; rewrote the stale plugin header. New regression test `CascadeDecompositionTest.NonRootCompositeLayersNotStreamedDirectly` (verified failing on the old code). Full suite green. **In-demo verification pending:** no z-fighting on undecomposed surfaces, silhouette refines per level (16 m → 4 m → 1 m), caves/soil cap present, decompose→evict→re-approach identical. Item 2's holes (fly-away bands) are now the most visible remaining P0. |
| 2026-06-12 | fixes-3 | 2 | Engine: bottom-up re-atomization. Drain caches each macro's block voxel (`CompositeLayerInfo::originalVoxel`); new `reatomize()` collapses a parent macro when its child chunks leave the child layer's eviction radius — cascade-evicts descendants, restores the cached block voxel, reports via `newlyAtomic`. Non-root LOD evict and budget pass route through it. Pinning upgraded to recursive `subtreePending` (jobs in flight anywhere below pin the collapse). Anti-thrash guard: a macro still inside the approach radius is never re-atomized (collapse range = max(child evict radius, approach radius)); without this, configs where approach radius > child evict radius decompose/collapse in a loop (caught by `DecompositionStateConsistentPerLayer`). Demo: `applyDiffs` now remeshes composite chunks for `newlyAtomic` macros. New regression test `BottomUpEvictionReatomizesParent` (decompose → mid-distance collapse, block restored, no hole, re-decomposes on re-approach; verified failing on the old code). Full suite green. **In-demo verification pending:** fly out to ~500 m, look back — coarse blocks restored, no holes at any band; fly back — identical re-decomposition. |
| 2026-06-12 | fixes-4 | 6, 7, 8, 9, 10, 11 | Engine: (7) completed jobs queue in a manager backlog and apply at most `applyPerFrame` per tick (new tick param, default 16, ≤0 = unlimited; backlog counts as `inFlight`). (8) approach trigger collects candidates and enqueues nearest-first with a fully deterministic (dist, layer, coords) sort. (6) the scan iterates resident chunks with whole-chunk AABB rejection instead of a voxel cube, gates ordered cheapest-first (direct-array solid read, distance, then hashes); frontier model left as future work. (9) per-chunk pending counters (`CompositeLayerInfo::pendingChunks`) + per-layer pending totals make eviction pinning O(1) in the common case (`chunkPinned`). (10) new `Layer::setVoxelNoDirty` used for the manager's block-voxel clear/restore so engine writes never dirty-pin chunks. (11) budget pass pins chunks inside the approach radius (soft cap in the bubble, hard outside — prevents budget/approach churn) and now also enforces a **terminal child layer's** `resident_chunk_budget` by force-collapsing farthest owning macros (the actual bgfx-handle protection); demo sets per-layer budgets (128/256/1024/2048), calls `setVerticalBand(0,0)` (new manager forwarder, root-chunk units), and keeps empty meshes out of the stores. New tests: `ApplyPerFrameBudgetSpreadsDrain`, `TerminalChildBudgetCollapsesFarthestMacros`. Full suite green. **In-demo verification pending:** no hitching while the cascade churns; nearest blocks decompose first; long flights stay under the handle cap. |

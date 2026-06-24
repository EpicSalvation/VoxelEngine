# M17 Discussion Task — Pre-Release Feature Sanity Check

**Status:** Discussion deliverable. This document **proposes nothing as decided.**
It catalogs candidate features, tools, and knobs for the initial (1.0) release so a
follow-up discussion can *accept*, *defer*, or *reject* each one. Accepted items
will then be injected as task lines into M17 (or a new pre-release milestone) in a
separate change. No code or behavior is changed by this document.

**The README task this answers (M17):**

> *Discussion task: perform a sanity check. Considering the current feature set,
> design philosophy, stated goals, and typical game engine behaviors and features,
> are there any other features, tools, or knobs that we should consider for the
> initial release of the engine? Add tasks to this milestone (or an additional
> milestone prior to release, if appropriate) to address accepted features and
> knobs.*

---

## 1. How this differs from the architecture gap audit

[`m17-architecture-gap-audit.md`](m17-architecture-gap-audit.md) asked an *internal*
question: **does the code match the engine's own spec (ARCHITECTURE.md)?** It found
the engine substantially as-built, with one real deferred feature (multi-level
propagation, now done) and some doc lag.

This document asks an *external* question: **independent of the current spec, what do
typical game engines — and specifically voxel engines — provide that a 1.0 of this
engine might be expected to provide, given its stated goals?** A gap here is not a
contradiction between code and doc; it is a capability the spec never promised but a
developer adopting the engine might reasonably miss.

The two overlap only where noted (e.g. the global memory-budget backstop, gap-audit
G10, resurfaces here as a performance knob).

---

## 2. The triage lens

Two principles bound what *belongs* in this engine, and every candidate below is
judged against them rather than against "what Minecraft has":

1. **Mechanism, not policy.** The engine's defining split (ARCHITECTURE §7/§17) is
   that the *engine detects and fires events and owns fields/overlays; plugins supply
   the response*. The engine never writes voxels from simulation. So the right
   question for a gameplay-flavored feature is rarely "should the engine *do* this?"
   but "does the engine expose the **seam** a plugin needs to do this?" Inventory,
   mobs, AI, quests, and weather are game/plugin concerns; the *collision body*,
   *lighting field*, and *particle submission path* they would build on are
   engine-tier.

2. **The four stated goals** (README): multi-scale beyond Minecraft; plugin-based
   extensibility; material-driven simulation; **AI-coding-agent friendliness**. A
   candidate that erodes goal 4 (e.g. a deep inheritance API, a hidden global) is
   suspect even if conventional.

**Disposition vocabulary used below:**

- **Accept (1.0)** — recommend doing it before release; engine-tier and reasonably
  expected.
- **Defer** — legitimate, but post-1.0 or a later milestone; record so it is a
  conscious schedule, not an oversight.
- **Non-goal** — deliberately out of scope; record the rationale so it reads as a
  decision.
- **Already tracked / verify** — already an M17/M18 line, or needs a quick
  code-check rather than new work.

> Recommendations are this audit's *opinion to seed discussion*, not decisions.

---

## 3. Candidate catalog

### A. Rendering & visual

The renderer is the area where the engine is furthest below "typical voxel engine"
expectations: shading today is **fixed directional brightness with no real lighting**
(`ChunkMeshData.cpp` — top faces full, sides/bottom dimmer, baked into vertex color),
there is no light propagation, and no ambient occlusion.

| ID | Candidate | Current state | Tier | Recommendation |
|----|-----------|---------------|------|----------------|
| **A1** | **Voxel lighting model** — propagated sky light + block (emitter) light, the single most recognizable missing voxel-engine feature | None; flat per-face shade only | Engine | **Accept (1.0)** — strongest recommendation |
| **A2** | **Ambient occlusion in the mesher** — per-vertex AO darkening at concave voxel corners ("smooth lighting") | None | Engine | **Accept (1.0)** — cheap, large visual win, pairs with A1 |
| A3 | **Time-of-day / directional sun** — sun direction + ambient as a renderer *policy* (mirrors the fog policy already planned, and the §18 gravity policy) | None | Engine seam + plugin policy | **Accept-as-knob or Defer** — couples to A1 |
| A4 | **Particle / transient-sprite seam** — a renderer path for short-lived points/quads (break debris, dust, splashes); today "falling debris" is whole voxels (M13 plugin) | None | Engine seam | **Defer** (or light Accept) |
| A5 | **Transparency / alpha-sort correctness** — translucent water exists (M14 `flow`); verify draw ordering is correct for overlapping translucent voxels | Translucent path exists; ordering unverified | Engine | **Verify**, then Accept-as-fix if needed |
| A6 | **Texture animation + mipmapping/filtering** — animated atlas frames (flowing lava/water), mip/aniso on the atlas | Static, point-sampled atlas (M15) | Engine | **Defer** |
| A7 | **View-frustum culling** — submit only chunks intersecting the camera frustum; today all chunks within the per-layer view distance are submitted | Distance/LOD culling only; no frustum test found | Engine | **Accept-as-cleanup** — fold into the M17 profiling pass |
| A8 | **Greedy meshing** — merge coplanar same-material faces into larger quads; standard voxel vertex-count optimization | Per-face culled mesher only | Engine | **Evaluate under profiling** — Defer unless profiling shows mesh cost dominates |
| A9 | **Anti-aliasing / post-processing seam** (MSAA toggle, tonemap hook) | None | Engine | **Defer** |

**Note on A1/A2 and the design philosophy:** lighting fits the engine's existing
pattern almost exactly. Fluid and thermal are *engine-owned sparse field overlays*
that plugins react to (§17); a **light level is the same shape of data** — an
engine-owned field, propagated by an engine solver, *sampled by the renderer*
(consumer), with emitter values supplied by materials/plugins (the `flow`/heat-source
precedent). This makes A1 a natural fit rather than a foreign body, which is partly
why it is the headline recommendation.

### B. Simulation & world

| ID | Candidate | Current state | Tier | Recommendation |
|----|-----------|---------------|------|----------------|
| **B1** | **Reusable kinematic-body / actor API** — the engine has exactly **one** kinematic body (the player), hand-built per demo on `VoxelCollision::moveAABB`. Mobs, items, vehicles, projectiles all need multiple gravity-aware bodies | Single-body, demo-owned | Engine | **Accept (1.0) — or explicit Non-goal.** Decide whether the engine offers a reusable body API or leaves all bodies to games |
| B2 | **Dynamic rigid-body physics** (tumbling debris, thrown objects) beyond axis-resolved AABB | None | Game/plugin | **Non-goal for 1.0** (record) |
| B3 | **Save-game / decomposition-state versioning + migration** — `.vxc` carries a version header, but is there a forward-migration path as the engine evolves past 1.0? | Versioned format; migration path unclear | Engine | **Accept-as-decision** — at minimum document the versioning/migration contract |
| B4 | **Global memory-budget backstop** (gap-audit G10) — eviction is per-layer chunk caps only; no global byte ceiling | Per-layer caps only | Engine | **Already flagged** to the M17 profiling task; confirm there |
| B5 | **Weather / wind fields** | None | Game/plugin | **Non-goal / plugin policy** |

### C. Input, UI, camera

| ID | Candidate | Current state | Tier | Recommendation |
|----|-----------|---------------|------|----------------|
| **C1** | **Input abstraction / action mapping** — rebindable actions, gamepad support; today every demo hand-rolls raw GLFW key/mouse polling | None | Engine seam (optional) | **Accept (thin helper) or Non-goal** — decide whether input mapping is engine- or game-owned |
| **C2** | **UI/HUD seam** — every demo hand-rolls bgfx debug-text HUDs; no menu/inventory/widget facility or documented immediate-mode-GUI integration | None | Engine seam (optional) | **Accept (documented seam) or Non-goal** |
| C3 | **Surface-normal camera orientation** | Tracked | Engine | **Already an M17 task** |

### D. Tooling & developer experience

| ID | Candidate | Current state | Tier | Recommendation |
|----|-----------|---------------|------|----------------|
| **D1** | **Leveled / structured logging** — `Log` exposes only `warn()` with a settable handler (`src/core/Logger.cpp`); no info/debug/error levels or categories | warn-only | Engine | **Accept (1.0)** — cheap, high developer-experience return |
| **D2** | **Engine-provided metrics surface** — frame time, draw calls, resident-chunk counts, decomposition queue depth, voice count; today each demo recomputes HUD stats by hand | None (ad-hoc per demo) | Engine | **Accept (1.0)** — complements the M17 profiling pass; also feeds C2 |
| D3 | **Settings/config persistence** — graphics quality, view distance, keybinds saved across runs | None | Engine/game | **Defer** (or minimal Accept) |
| D4 | **Asset hot-reload** (shaders, textures) for iteration | None | Engine | **Defer** |
| **D5** | **Plugin ABI-version stamp + defensive load** — native plugins share the host address space; a stale/mismatched plugin can corrupt or crash the engine with no guard | No version check found | Engine | **Accept (1.0)** — small, protects every consumer and the AI-agent-friendliness goal |

### E. Content & assets

| ID | Candidate | Current state | Tier | Recommendation |
|----|-----------|---------------|------|----------------|
| **E1** | **`.qb` (Qubicle) import/export** — **the README overview and interop table claim "Full import/export" for `.qb`, but no Qubicle handler exists in code** (only `.vox`) | Claimed, **not implemented** | Engine | **Accept (implement) OR correct the README** — a release-honesty gap, like the M15 textured-interop note |
| E2 | **Scripting tier** (Lua/JS) for non-C++ modders | None; C++ plugin ABI only | Engine | **Likely Non-goal for 1.0** — the flat C++ ABI is a deliberate, AI-agent-friendly choice; record the decision |
| E3 | **Unified asset/resource manager** — textures, audio, `.vox`, `.bbmodel` each load ad-hoc per subsystem | Per-subsystem | Engine | **Defer / evaluate** — may be fine given the plugin model |

### F. Networking

| ID | Candidate | Current state | Tier | Recommendation |
|----|-----------|---------------|------|----------------|
| F1 | **Auth / encryption / anti-tamper** — M11 is explicitly unauthenticated UDP | By design for M11 | Engine | **Defer (post-1.0)** — record as known limitation |
| F2 | **Dedicated-server mode** (authority without a local player) vs. host-as-authority only | Host-as-authority only | Engine | **Defer / evaluate** |

### G. Platform & distribution

| ID | Candidate | Current state | Tier | Recommendation |
|----|-----------|---------------|------|----------------|
| G1 | **Asset bundling / packaging story** for a shipped game (how a game ships its plugins + assets) | None | Engine/tooling | **Defer to M18** (templates) — flag the dependency |
| G2 | **Additional backends** (D3D12/DXIL, Wayland, WGSL) | Deferred | Engine | **Already recorded** (gap-audit G9) — leave deferred |

---

## 4. Recommended shortlist for the discussion

If the discussion wants a starting position, this audit's ranked "**Accept for 1.0**"
shortlist — engine-tier, reasonably expected, and consistent with the design
philosophy — is:

1. **A1 — Voxel lighting model** (sky + block light). The biggest perceived gap vs.
   any voxel engine, and a clean fit for the existing field-overlay pattern.
2. **A2 — Ambient occlusion in the mesher.** Cheap, large visual return; pairs with A1.
3. **E1 — `.qb` support *or* a README correction.** Release honesty: the engine
   currently claims a format it does not implement.
4. **D1 — Leveled logging** and **D2 — engine metrics surface.** Low-cost
   developer-experience wins that also support the profiling pass and any future HUD.
5. **D5 — Plugin ABI-version guard.** Small, protects every consumer.
6. **B1 — A decision on the reusable kinematic-body API** (build it, or declare bodies
   a game concern). Currently the single-body assumption is implicit; make it explicit.
7. **A7 — Frustum culling** and the **C1/C2** input-and-UI seam *decisions* (do, or
   declare game-owned).

Everything else is a reasonable **Defer** or **Non-goal** — recorded above so each
reads as a conscious call rather than a miss.

## 5. Bottom line

The engine's *simulation and architecture* core is mature and largely as-built. The
candidates that rise to the top are concentrated in **(a) rendering** — lighting and
AO are the conspicuous absences for a voxel engine — and **(b) a thin band of
developer-experience and release-honesty items** (logging, metrics, the ABI guard,
the `.qb` claim). The gameplay-flavored gaps (entities, scripting, UI) are best
resolved not by the engine implementing them but by **deciding and documenting which
seams the engine owns** versus what it leaves to games — which is itself the
discussion this document is meant to open.

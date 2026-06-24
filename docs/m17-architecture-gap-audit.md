# M17 Design Task — Architecture / Implementation Gap Audit

**Status:** Design deliverable (scopes the rest of M17). No code or behavior is
changed by this document.

**Scope:** The README M17 design task asks us to *"identify gaps between current
implementation and full architecture spec."* This audit walks
[`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §1–§18 against the tree as it stands at
the close of M16 and catalogs every place the document and the code disagree —
whether the code is behind the spec, the spec is behind the code, or a divergence
is a deliberate-but-untracked deferral.

It is the companion to two other M17 line items and feeds both:

- *"ARCHITECTURE.md fully reflects implemented behavior (not just intended
  behavior)"* — every **Class A** entry below is a doc-currency fix it must make.
- *"Example plugin suite covers all major hook types"* — **G11** scopes exactly
  which hook types are currently unexercised in-tree.

---

## 1. How to read this catalog

The architecture document is, unusually, **mostly as-built**: §1–§10 and §13–§14
were rewritten from aspirational to as-implemented as each milestone landed, and
the M13/M14/M16 sections (§7, §17, §18) were authored against shipped code. So the
gaps are not a long tail of unimplemented promises — they cluster into three
classes, and naming the class is most of the work:

- **Class A — Doc currency.** The capability is *implemented and shipping*, but the
  prose still reads in the future tense it was written in ("Files (planned)",
  "Implementation details will be added", "Consumers arrive in Mxx"). The code is
  ahead of the doc. Fix: rewrite the prose to as-built. Zero code change.
- **Class B — Tracked deferral, untracked in M17.** The spec *explicitly* calls
  the gap out as deferred ("revisit in M17", "a deferred refinement", "a planned
  follow-up"), the code agrees, but the deferral has **no M17 task line** to force
  a decision. Fix: inject a work item (to do it, or to consciously re-defer with a
  recorded rationale).
- **Class C — Spec ahead of code.** The spec describes a behavior or surface the
  code does not yet provide, *without* flagging it as deferred — a silent gap a
  reader of the doc would assume is present. These are the ones worth finding.

Each entry: **Where** (doc §/line + code file:line) · **Spec says** · **Code does**
· **Class** · **Disposition**.

---

## 2. What is already consistent (not a gap)

So the catalog is read as exceptions, not the rule — these were spot-checked and
the doc matches the code:

- **§1 Coordinate system / floating origin** — `WorldCoord` is the only world-space
  type; `toLocalFloat` is the single GPU/audio submission seam. As described.
- **§4 Cascading decomposition + §4 engine-owned orchestration** — `DecompositionManager`
  holds a `DecompositionState` per composite layer and drives the approach→drain→
  evict loop; cascading eviction is implemented. As described.
- **§5 Material property consumption contract** — `RemovalModel`/`RemovalAccumulator`
  read properties off the voxel by value; the `hardness` sentinels behave per the
  table. As described.
- **§7 support-potential flood** (detection half) — axis-free flood, anchor rule,
  deterministic sorted-coord order, `tuning::physics` budgets with carried overflow.
  As described (the **one** exception is the multi-level chain, **G1**).
- **§8 plugin ABI** — every hook the §8 list and the M11/M12/M13/M14 tables name is
  present in `include/plugin_api.h` as a flat-POD, append-only `PluginContext`
  function pointer (verified against the `register_*`/`Fn` typedef set). No `std::`
  type crosses the boundary.
- **§16 audio, §17 fluid/thermal, §18 gravity provider** — the three seams
  (`gravityAt`, the field overlays, the audio side-table) exist and match their
  sections' as-built bodies. The §18 consumers (collision grounding, fluid drain,
  face roles) all read the supplied gravity vector.

The takeaway: this is a **polish** audit, not a "the engine doesn't match its
design" audit. Most gaps are doc lag (Class A) and consciously-parked items
(Class B). There is exactly one Class C of real substance (**G1**).

---

## 3. Gap catalog

### G1 — Upward damage propagation resolves a single composite level only `[Class B → promote]`

- **Where:** ARCHITECTURE §7 (line 310); `src/simulation/PropagationSystem.cpp:198-200`
  (the in-code `M17 TODO`), `PropagationSystem.h:49`.
- **Spec says:** stability aggregation "resolves at a **single composite level**
  (child edits → immediate parent → neighbor cascade at that level). Re-aggregating
  further ancestors up the chain (reusing the M10 cascade infrastructure) is
  deliberately deferred — **revisit in M17 (Polish and Release)**."
- **Code does:** exactly that — `findUnstable` builds its candidate set from the
  dirty macro plus its solid 6-neighbors at one level, with a literal `M17 TODO`
  comment. A deep stack (e.g. demo 10/17's `macro→micro→grid`) never re-evaluates a
  grandparent macro's stability when a grandchild edit hollows it out.
- **Class:** **B** — this is *the* item M13 explicitly handed to M17, yet **the M17
  milestone has no task line for it.** That is the single most important finding of
  this audit: the one consciously-deferred-to-M17 engine behavior is currently
  untracked in M17.
- **Disposition:** **Inject an M17 implementation task.** Either (a) implement the
  multi-level chain — when an aggregate goes dirty, walk it up the ancestor
  coordinate chain the M10 cascade already computes and re-aggregate/re-flood at
  each composite level, bounded by the same `tuning::physics` budgets so a deep
  chain reaction still spreads across frames; or (b) make a recorded decision that
  single-level is sufficient for release and downgrade the in-code TODO to a
  permanent documented limitation. (a) is the spec's stated intent. This is the
  highest-value M17 engine work item.
- **Resolved (M17):** implemented option **(a)**. `PropagationSystem` discovers the
  full composite chain and every aggregate/flood operation is level-parameterized;
  a coarse macro aggregates its child macros' aggregates recursively, and
  `recomputeAggregate(level, …)` marks the next-coarser parent dirty so
  `PhysicsSystem::tick` re-floods every ancestor level fine→coarse within one tick
  under a shared `tuning::physics` budget (per-level carry/fired sets). N=1 stays
  byte-identical; `tests/MultiLevelPropagationTest.cpp` covers the `macro→micro→grid`
  chain. The in-code `M17 TODO` is retired and ARCHITECTURE §7 updated. The behavior
  is currently test-only — no demo combines a deep composite stack with
  `PhysicsSystem` (demo 13 has physics but one level; demo 10 has a deep chain but no
  physics) — so a follow-up M17 task tracks building a demo that shows the
  grandparent cave-in.

### G2 — Public-header surface is not finalized `[Class B → decide]`

- **Where:** ARCHITECTURE §12 "Not Yet Finalized" (lines 589–591); `include/` holds
  only `WorldCoord.h` + `plugin_api.h`.
- **Spec says:** "Consumer-facing types such as `Engine`, `LayerConfig`, and
  `PluginManager` still live under `src/` and are reached by in-tree demos directly.
  Promoting the genuinely public ones into `include/` — and exposing the renderer
  behind a creation factory so bgfx stays entirely out of the public API — is a
  planned follow-up."
- **Code does:** confirmed — every front-end type a real out-of-tree game would
  need (`Engine`, `LayerConfig`, the `Renderer` interface) is under `src/`, reachable
  only by the privileged in-tree demos (§12). A third-party game cannot today build
  against the engine without reaching into private headers.
- **Class:** **B** — explicitly a "planned follow-up", untracked. This is a genuine
  pre-1.0 gate: §12 frames `include/` as "the committed public API", but the
  committed surface is currently too thin to write a game against.
- **Disposition:** **Inject an M17 (or M19 "verify docs/finalize") task** to make the
  public-surface decision: which `src/` types graduate to `include/`, and add the
  renderer creation factory that keeps bgfx private. Cross-references the M18
  "template series" (a boilerplate game needs a real public surface to link).

### G3 — §15 Networking and §16 Audio still read as pre-implementation `[Class A]`

- **Where:** ARCHITECTURE §15 (lines 725, 729) and §16 (lines 827, 831).
- **Spec says:** both open with **"Files (planned):"** and **"This section records
  the design decisions made for Mxx. Implementation details will be added as the
  milestone is built out."**
- **Code does:** M11 and M12 are **complete and shipped** — `src/net/` and
  `src/audio/` are fully populated, demo 11 (shared world) and demo 12 (soundscape)
  exercise them, and the test suite covers both. The "(planned)" framing is stale.
- **Class:** **A** — doc currency. A reader is told these subsystems are designs on
  paper when they are running code.
- **Disposition:** **Doc fix** under the "ARCHITECTURE fully reflects implemented
  behavior" task: drop "(planned)" from the Files lines, replace the
  "will be added as built out" sentences with as-built bodies, and rename
  "Design Decisions (M11/M12)" to past tense — matching how §7/§17/§18 already read.

### G4 — §7 and §17 say "Consumers arrive in M13/M14" `[Class A]`

- **Where:** ARCHITECTURE §7 (line 284: "Consumers arrive in M13; the design below
  is the M13 contract") and §17 (line 916: "Consumers arrive in M14").
- **Spec says:** the consumer systems are forthcoming.
- **Code does:** `PhysicsSystem`/`PropagationSystem` (M13) and
  `FluidSystem`/`ThermalSystem` (M14) are implemented, wired into the frame loop,
  and tested. The mandatory `crumble`/`flow` plugins ship.
- **Class:** **A** — doc currency (same family as G3, milder).
- **Disposition:** **Doc fix** — change to "Consumers landed in M13/M14".

### G5 — §11 chunk size documented as "default TBD" `[Class A]`

- **Where:** ARCHITECTURE §11 (line 518: "size is a tunable constant, default TBD");
  `src/world/Chunk.h:38` — `Chunk(ChunkCoord, int sizeVoxels, WorldCoord)`.
- **Spec says:** the chunk dimension's default is To Be Determined.
- **Code does:** chunk size is a resolved per-layer value (`sizeVoxels`, fed from the
  layer config), not an open question. The "TBD" is a leftover from before the
  layer-driven sizing landed.
- **Class:** **A** — doc currency.
- **Disposition:** **Doc fix** — state that chunk size is a per-layer config value
  (and cite the default the configs actually use), retiring "TBD".

### G6 — §15/§16 "Files (planned)" path list (folded into G3)

Tracked under **G3**; called out separately only so the doc-fix pass does not miss
the `Files (planned):` header lines in addition to the body prose.

### G7 — Exposure-aware boundary overrides are not implemented `[Class B, fine to leave]`

- **Where:** ARCHITECTURE §6 (line 263).
- **Spec says:** boundary overrides paint a macro's *geometric* outer faces, "**not**
  neighbor-exposed faces … Exposure-aware boundaries (painting only faces that
  actually meet empty space) are a **deferred refinement**."
- **Code does:** matches — the decomposition worker is a pure function of one
  macro's inputs and never samples neighbor occupancy (a §13 boundary), so a buried
  face still gets the boundary skin.
- **Class:** **B** — explicitly deferred, and the deferral is *correct*: making it
  exposure-aware would require the worker to read neighbor occupancy, crossing a
  deliberate dependency boundary. Low player-visible impact (buried faces are
  culled anyway).
- **Disposition:** **Leave deferred; no M17 task.** Recorded here so it is a
  conscious non-goal rather than an oversight. Revisit only if a demo needs it.

### G8 — Blockbench import is one-way; no exporter / round-trip `[Class B, fine to leave]`

- **Where:** ARCHITECTURE §9 (line 444).
- **Spec says:** the Blockbench importer is "one-way import (exporter/round-trip and
  per-face sub-UV sheets are follow-ons)."
- **Code does:** `plugins/blockbench` registers an importer only; no exporter.
- **Class:** **B** — explicitly a follow-on.
- **Disposition:** **Leave deferred.** Belongs more naturally with the M18
  "custom voxel assets" tutorial work than M17 polish. No M17 task; noted so M18
  picks it up.

### G9 — Renderer backend follow-ups: D3D12/DXIL, Wayland, WGSL `[Class B, fine to leave]`

- **Where:** ARCHITECTURE §9 (lines 401, 478–479); `src/renderer/BgfxRenderer.cpp:13,16`
  (`BGFX_PLATFORM_SUPPORTS_DXIL 0`), `src/platform/Window.cpp:32-34`.
- **Spec says:** D3D12 (DXIL) deferred (shaderc `--werror` issue; bgfx prefers D3D11
  on Windows anyway); Wayland native-handle wiring is a planned follow-up (runs
  through XWayland today); WGSL "not a current target".
- **Code does:** matches each — DXIL profile not generated, X11 forced, no WGSL.
- **Class:** **B** — all three explicitly deferred with sound rationale.
- **Disposition:** **Leave deferred.** None blocks a Windows/Linux-X11/macOS release.
  No M17 task; the rationales already live in §9.

### G10 — Deferred persistence / memory-budget backstops `[Class B, fine to leave]`

- **Where:** ARCHITECTURE §11 (line 527: decomposition-state not persisted; line 535:
  global estimated-byte budget deferred).
- **Spec says:** (a) "which composites are decomposed" is not persisted — a load-time
  optimization deferred to a later save-game milestone, not a correctness issue;
  (b) the global estimated-byte eviction budget is a deferred outer backstop over
  the per-layer chunk caps.
- **Code does:** matches — re-decomposition on load is deterministic and transparent;
  eviction is driven by per-layer chunk caps only.
- **Class:** **B** — both deferred *with the reason recorded* and neither a
  correctness gap.
- **Disposition:** **Leave deferred.** Flag the global byte budget as a candidate for
  the M17 *performance profiling* task to either justify or schedule — profiling is
  exactly what would reveal whether a lopsided-density config needs it.

### G11 — Example plugin suite does not cover all hook types `[Class C → already an M17 task]`

- **Where:** the named M17 task "Example plugin suite covers all major hook types";
  `src/plugins/ExamplePlugin.cpp` (the §8-referenced "worked example") registers only
  `register_material` + `register_layer_generator`. Coverage is otherwise spread
  thinly across `plugins/`.
- **Spec says (§8):** the full hook set is the plugin contract; `ExamplePlugin` is
  "a worked example."
- **Code does:** across **all** in-tree plugins, these hook types are **never
  registered by any example**: `register_on_thermal_event`,
  `register_on_chunk_created`, `register_on_chunk_evicted`, `register_noise`,
  `register_exporter`. So a thermal-response plugin, a chunk-lifecycle plugin, a
  custom-noise provider, and an exporter plugin have **no reference implementation** a
  developer can copy — even though all four hooks are first-class in `plugin_api.h`
  and §6/§8.
- **Class:** **C** — spec presents these as supported with worked examples; the
  worked examples don't exist.
- **Disposition:** **Already an M17 task — this entry scopes it.** The suite work
  should add minimal reference plugins (or extend `ExamplePlugin`) for at least the
  five uncovered hooks above. `register_noise` is especially worth a sample: §6
  sells noise as a pluggable facility but nothing in-tree *provides* a custom noise.

### G12 — Renderer view is hard Y-up vs. the §18 gravity-agnostic kinematics `[Class C → already an M17 task]`

- **Where:** ARCHITECTURE §18 (gravity-relative grounding/faces) vs. the renderer's
  hard `(0,1,0)` view-up in `BgfxRenderer::render`; already the M17 "Surface-normal
  camera orientation" task.
- **Spec says:** §18 makes "down" configurable through every *simulation* consumer.
- **Code does:** the *renderer* still assumes Y-up, so a player standing on an
  asteroid's +X face is physically grounded but sees a tilted horizon (the
  asteroid-belt demo's documented caveat).
- **Class:** **C**, but **already tracked** by the M17 camera task and the M16
  generality audit's G3 note. Listed here only for completeness so the gap set is
  exhaustive; no new task needed.
- **Disposition:** Covered by the existing M17 camera-orientation line item.

---

## 4. Summary & disposition table

| ID | Gap | §  | Class | Disposition |
|----|-----|----|-------|-------------|
| **G1** | Upward propagation is single-level only | 7  | **B** | **Done (M17)** — multi-level ancestor-chain re-aggregation implemented; `MultiLevelPropagationTest` covers it. |
| **G2** | Public-header surface not finalized | 12 | **B** | **New M17/M19 task** — promote public types + renderer factory. Pre-1.0 gate. |
| G3 | §15/§16 read as pre-implementation | 15,16 | A | Doc fix (feeds "ARCHITECTURE reflects implemented behavior"). |
| G4 | "Consumers arrive in M13/M14" | 7,17 | A | Doc fix. |
| G5 | Chunk size "default TBD" | 11 | A | Doc fix. |
| G7 | Exposure-aware boundaries deferred | 6  | B | Leave deferred (correct non-goal). |
| G8 | Blockbench export / round-trip | 9  | B | Leave deferred → M18. |
| G9 | D3D12 / Wayland / WGSL | 9  | B | Leave deferred (rationale already recorded). |
| G10 | Decomp-state + byte-budget backstops | 11 | B | Leave deferred; byte budget → revisit under M17 profiling. |
| **G11** | Example suite misses 5 hook types | 8  | **C** | **Existing M17 task** — this entry scopes the five: `on_thermal_event`, `on_chunk_created`, `on_chunk_evicted`, `register_noise`, `register_exporter`. |
| G12 | Renderer hard Y-up vs §18 | 18 | C | Existing M17 camera task. |

---

## 5. Recommended M17 work-item injections

The design task's job is to scope, not to build. Two gaps are **new work not yet on
any milestone** and should become M17 task lines; the rest are either already-tracked
M17 tasks (G11, G12), doc-fixes folded into the existing "ARCHITECTURE reflects
implemented behavior" task (G3, G4, G5), or conscious deferrals to leave alone
(G7, G8, G9, G10):

1. **Multi-level upward damage propagation (G1)** — the one engine behavior M13
   explicitly handed to M17 and the only substantive Class-C/B gap in the
   simulation core. Implement the ancestor-chain re-aggregation, or make and record
   a permanent single-level decision. *This is the headline M17 engineering item.*
2. **Finalize the public-header surface (G2)** — promote `Engine`/`LayerConfig`/the
   `Renderer` factory into `include/` (or consciously schedule it to M19), so a
   third-party game can link the committed API the §12 framing already promises.

Everything else is doc currency (the existing ARCHITECTURE-currency task absorbs
G3–G5), already-scoped M17 tasks (G11, G12), or correctly-parked deferrals
(G7–G10) recorded here so they read as conscious non-goals rather than misses.

**Bottom line:** the engine is substantially as-built against its own spec. The
gaps are one real deferred engine feature (multi-level propagation), one pre-release
API-surface decision, a handful of stale future-tense doc sections, and a thin
example-plugin tail — exactly the shape a "pre-release polish" milestone should
have.

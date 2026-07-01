# M18 Mega-Demo — "AI friendliness" build metrics

This document records concrete metrics for building the M18 Mega-Demo (`demos/20-mega-demo`,
plus the `overworld`, `trees`, and `mob` plugins) end to end with an AI coding agent
(Claude Code). The goal is to show the engine's *AI-friendliness* with numbers rather than
assertion: a codebase that an agent can extend cleanly produces few refinement passes, few
ABI re-reads, clean-on-first-build compiles, and a high reuse ratio.

The numbers below are the *actual* counts from the build session (planning + implementation),
not estimates. Token/cost figures come from the CLI's own `/cost` accounting and are recorded
separately where exact tokens aren't surfaced to the agent.

## Phase metrics

| Metric | Planning | Implementation | Notes |
|---|---|---|---|
| Subagent explorations | 3 (one parallel batch) | 0 | Planning fan-out only |
| Files read for ground truth | 5 + 1 grep sweep | ~14 (APIs, demos 03/12/13/14/15/18, CMake, headers) | Reading to learn the API, not to fix mistakes |
| Clarifying-question rounds | 1 (3 questions, answered in one pass) | 0 | Scope locked up front |
| Plan revisions | 2 (draft → +metrics deliverable) | — | |
| **Compile-error fix cycles** | — | **0** | Every C++ target compiled on the **first** attempt |
| Integration/runtime fix cycles | — | 1 | A CMake source-property *scoping* leak (`set_source_files_properties` is directory-scoped, so a test-only `COMPILED_IN` define leaked onto the disk MODULE and suppressed its `voxel_plugin_init`); caught by the demo smoke test, fixed in one edit. **A build-system quirk, not an engine-API mistake.** |
| Warnings to clear (W4) | — | 1 (unused param in `trees`) | Fixed in one edit |
| `plugin_api.h` re-reads to fix an ABI mistake | 0 | 0 | The host↔plugin mechanism resolved from the existing shared-table pattern on the first look; zero ABI compile errors |
| Build iterations to green | — | 3 (plugins; demo Stage A; demo + M14) — each clean | No C++ red→green debugging loops |
| Runtime smoke tests | — | 4 (seed 12345; +M14 fluid; +rename; +CMake fix) — no crash, atlas built, all plugins loaded | |
| New automated tests | — | 4 (seed determinism, seamlessness, layered terrain) — all green; full suite 494 pass | `tests/MegaDemoDeterminismTest.cpp` |

### The headline AI-friendliness signal

Across three new plugins (`overworld`, `trees`, `mob` + `mob.h`) and an 856-line demo
front-end wiring **ten** engine subsystems, there were **zero compile-error fix cycles** — every
target built clean on the first attempt, including the second pass that added the M14 fluid
integration. The only diagnostic at all was a single unused-parameter warning. This is the
practical payoff of the engine's design choices: a flat C plugin ABI documented in one header
(`include/plugin_api.h`), a uniform "one `plugins/<name>/plugin.cpp`" + auto-GLOB build, and a
consistent shared-API-table pattern (`kinematic_body.h`) that the new `mob.h` copied verbatim.

## Output / leverage

| Metric | Value |
|---|---|
| New source files | 5 (`overworld/plugin.cpp`, `trees/plugin.cpp`, `mob/plugin.cpp`, `mob/mob.h`, `20-mega-demo/main.cpp`) |
| New lines of code | ~1,550 |
| Existing plugins reused **unchanged** | 6 (`kinematic-body`, `keyboard-mouse`, `gamepad`, `water`, `material-audio`, `flow`) |
| Engine primitives reused via the ABI | `move_aabb`, `register_on_tick`, feature/layer/material/texture/sound/fluid registries, `apply_edit`, palette/fog/atlas/HUD renderer seams |
| Engine subsystems exercised by one demo | M1–M5, M8, M9-noise, M12 (audio), M14 (fluid), M15 (textures), M16 (fog), M17 (kinematic body, input, HUD, lighting) |
| CMake edits to wire it all | 2 small blocks (path defines + one compiled-in stanza) — the GLOB build needed no per-file edits |

The reuse ratio is the story: the demo's distinctive *new* logic is the worldgen
(`overworld`), the tree placement (`trees`), and the zombie AI (`mob`). Everything else —
player physics, input, audio routing, water, fluid realization, textures, HUD — is composed
from plugins and engine seams that already existed, with no edits to them.

## Token / cost / wall-clock

Figures from the CLI's `/cost` for the full session (planning + implementation + the
verification loop), as reported at session close:

| Metric | Value |
|---|---|
| Total cost (USD) | **$74.00** (see subscription context below) |
| Compute time (API duration) | 2 h 13 m |
| Wall-clock span | 1 d 14 h (includes idle time between the planning conversation and the build session — not active work) |
| Code changes counted by the session | 5,598 lines added, 205 removed (session-wide, incl. the plan file and edit churn; the *net new shipped* source is ~1,550 LOC across 5 files) |

Token usage by model (input / output / cache-read / cache-write):

| Model | Input | Output | Cache read | Cache write | Cost |
|---|---|---|---|---|---|
| claude-opus-4-8 | 63.1k | 513.6k | 84.5M | 1.8M | $73.41 |
| claude-haiku-4-5 | 55.2k | 24.8k | 1.7M | 160.4k | $0.59 |

> **Cost context (important for interpreting the dollar figure).** This work was done on a
> **Claude Pro subscription**, not metered pay-as-you-go API billing. The $74.00 is the API-
> equivalent value `/cost` attributes to the session; against the subscription it consumed only
> **~35% of the single-session usage cap** — i.e. the entire mega-demo (three plugins, an
> 856-line demo, tests, and docs) fit comfortably inside one Pro session with headroom to spare,
> at no marginal dollar cost beyond the flat subscription. Date matters for any AI-cost metric:
> this was **late June 2026**, using **Claude Opus 4.8** (with Haiku 4.5 for light subtasks).
> Model capability, pricing, and subscription caps move fast, so read these numbers as a snapshot
> of that point in time, not a fixed cost of building comparable work.

Reading the numbers: the heavy **cache read** (84.5M) relative to fresh input (63.1k) shows
most of the engine context was served from cache across turns rather than re-read — the
practical effect of a codebase whose conventions are discoverable once (the single
`plugin_api.h` ABI header, the uniform plugin/demo layout) and then reused. Output tokens
(513.6k) dominate cost: this was a *write-heavy* task (three plugins + an 856-line demo +
tests + docs) rather than an exploration-heavy one, which is the desired shape — the agent
spent its budget producing code, not hunting for how the engine works.

## A note on scope honesty (updated post-M18.5)

The original M18 mega-demo was built on a single **terminal heightmap layer**. M18.5 briefly
added a **hybrid composite stack** (a coarse "blocks" layer over the "terrain" layer, plus an
immutable "bedrock" anchor) so `PropagationSystem` could aggregate children and drive M13
structural collapse — mining under an overhang triggered a real cave-in via the `crumble`
plugin.

**That collapse feature has since been removed from the mega-demo and is marked
experimental.** On a large streamed surface the support-flood mis-fires: ordinary surface
mining is misread as an unsupported span and triggers premature cave-ins, and running the
detection flood every frame hurt performance. Rather than ship a headline feature that
degrades the core mine/build/explore loop, the mega-demo is back to a single terminal
"terrain" layer, and collapse polish is a **post-1.0 goal**. The feature still lives in the
engine and is demonstrated in isolation by `demos/13-structural-collapse` and
`demos/19-multilevel-collapse`.

What the mega-demo keeps: the rolling heightmap surface, 3D-carved **caves** and ore, M14
fluid, M15 textures, and melee **combat** against mobs (`mob::api().attack_nearest`) — a real
mine/build/fight/explore survival slice.

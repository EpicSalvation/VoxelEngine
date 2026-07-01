# M17 Standard Skybox Support — Evaluation & Deliverable

**Status:** Evaluation deliverable + implementation. Records the gap analysis, the
design decision, and what shipped for the M17 task:

> *Standard skybox support (renderer): evaluate whether the renderer needs a
> first-class skybox seam — a cubemap or procedural sky, distinct from the flat
> clear-color + distance-fog policy already in place (§9) — and add it if the
> evaluation finds a gap. Needed groundwork for the planned M19 "No Man's Voxel"
> demo, where the player leaves a world's local bounds and needs a convincing
> sky/space backdrop rather than the flat clear color.*

---

## 1. The evaluation: is there a gap?

Before this milestone the renderer's entire background repertoire was:

- **`Renderer::setClearColor(rgb)`** — a single flat color the framebuffer is
  cleared to each frame (`BgfxRenderer`, ARCHITECTURE §9). One color, everywhere.
- **`Renderer::setFog(FogParams)`** — distance obscurance that fades *geometry*
  toward the fog color as it recedes (`include/renderer/Fog.h`). Fog paints
  nothing where there is no geometry; it only tints what is already drawn, and it
  is tuned to fade toward the *same* flat clear color so silhouettes dissolve into
  the background without a halo.

Together these answer "what color is the void behind the world?" with exactly one
color. They **cannot** produce:

- a **horizon** — a different color looking up vs. level vs. down;
- a zenith→horizon **gradient** (the single most recognizable feature of a sky);
- a distinct **space backdrop** that reads as "open space" rather than "a gray
  wall at the far plane."

The planned **M19 "No Man's Voxel"** demo is the forcing function: the player
flies *up and out* of a world's local bounds into open space between worlds. With
only a flat clear color, leaving the atmosphere looks like the world dissolving
into a uniform fill — there is no sky and no space. **The evaluation finds a real
gap**, exactly the one the task anticipated. So a first-class sky seam is added.

## 2. The design decision: procedural gradient, not a cubemap (yet)

The task offered two shapes — "a cubemap or procedural sky." The choice, in
keeping with the engine's mechanism-vs-policy philosophy (fog §9, gravity §18,
authority §15):

**A procedural view-direction gradient, drawn as geometry through the existing
voxel shader.** Concretely:

- **Mechanism (engine).** `Renderer::setSky(SkyParams)` takes a **zenith**,
  **horizon**, and **ground/nadir** color plus a horizon-band `falloff`. The
  renderer draws a **camera-centered sphere** whose per-vertex color is
  `skyColor(dir, up)` — a vertical gradient between the three stops, measured
  against the **camera up** vector (`setCameraUp`), so the horizon stays level on
  an arbitrary surface (the same gravity vector §18 already threads through the
  camera and shade ramp). See `include/renderer/Sky.h`.
- **Policy (game/plugin).** The colors, the band width, and any animation are
  supplied per frame by the game — typically the **`procedural-sky`** reference
  plugin, which ships three presets (atmospheric **day**, warm **dusk**, and a
  near-uniform dark **space** backdrop) and an optional **day/night cycle**. This
  mirrors the two fog suppliers (atmospheric-mist, range-attenuation) exactly.

**Why procedural, not a cubemap, for 1.0:**

1. **No new asset pipeline.** A cubemap needs six textures (or one cube texture)
   loaded, owned, and torn down — a whole content path. A gradient needs no assets.
2. **No new shader / no shader toolchain.** The sky is drawn as ordinary geometry
   through the **existing** `vs_voxel`/`fs_voxel` program (the sphere samples the
   built-in 1×1 white tile, so the per-vertex color passes straight through).
   Adding a `samplerCube` shader would mean regenerating committed bytecode for
   every backend via `shaderc` (ARCHITECTURE §9, "Shaders") — a heavy,
   toolchain-gated step this seam avoids entirely.
3. **It covers both cases M19 needs.** By choice of colors the same gradient is an
   atmospheric planet sky *or* a space backdrop — the two things "No Man's Voxel"
   switches between as you leave a world.
4. **It's a floor, not a ceiling.** Like the HUD seam (§9) and the fog seam, it is
   deliberately a thin mechanism a game builds on, not a full sky renderer.

**Recorded follow-ons** (out of scope for 1.0, the same way the fog task left
richer suppliers for later):

- **Textured cubemap sky** — an actual starfield/nebula image behind the gradient,
  for a truly photographic space backdrop. Needs a `samplerCube` shader + a cubemap
  asset path.
- **Sun / moon / star layer** — a bright disc in a direction, and hashed point
  stars. A per-vertex gradient is too coarse for a crisp sun; this wants either a
  second small shader or additive sprites (which also want the deferred
  particle/transient-sprite seam, Post-1.0 A4).
- **Direction-varying fog color** — today fog fades toward one flat color; against a
  gradient sky, far geometry ideally fades toward the sky color *in that direction*.
  Couples to the time-of-day/directional-sun item (Post-1.0 A3).

## 3. What shipped

- **`include/renderer/Sky.h`** — `SkyParams { enabled, zenith, horizon, ground,
  horizon_falloff }` (default `enabled == false`) and `skyColor(params, dir, up)`,
  the exact CPU mirror of the gradient the renderer bakes per vertex (the analog of
  `fogFactor()` in `Fog.h`).
- **`Renderer::setSky`** — the seam, default no-op; **`BgfxRenderer`** implements
  it. A unit sky sphere is tessellated once at init (index buffer static; vertex
  colors recomputed into a transient buffer each frame from the current params +
  camera up). It is drawn **camera-centered** at `0.999·farClip` with
  `DEPTH_TEST_LEQUAL` and **no depth write**, so correctness is by depth *test*
  alone — independent of draw order and backface culling (the sphere is centered on
  the eye, so every forward ray hits exactly one surface point and the far
  hemisphere is behind the near plane). Fog is forced off for the sky submit so the
  background is never self-fogged.
- **`plugins/procedural-sky/`** — the reference policy supplier (shared
  `procedural_sky.h` + `plugin.cpp`), registering no engine hooks, filling a
  `psky::api()` table with `sample(t)` / `configure(cfg)` and shipping `daySky()` /
  `duskSky()` / `spaceSky()` presets plus a day/night cycle.
- **`tests/SkyTest.cpp`** — pins the gradient curve (stops hit exactly along the up
  axis, tracks the camera up, safe on degenerate input, monotone between stops) and
  the plugin (defaults, static-by-default, the dark space preset, the animated
  cycle, and `configure`).
- **Docs** — ARCHITECTURE §9 gains a "Sky / Background" subsection;
  `docs/configuration-guide.md` documents the seam.

**The default is byte-identical.** `SkyParams::enabled` is `false`, so the renderer
skips the sky sphere entirely and the flat clear color shows through exactly as
before; a host that never calls `setSky()` is unchanged. Demo wiring lands with the
M19 "No Man's Voxel" demo this is groundwork for (the same way the reference input
plugins shipped with tests ahead of their demo).

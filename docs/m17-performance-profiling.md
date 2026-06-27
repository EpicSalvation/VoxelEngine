# M17 Performance Profiling Pass

**Status:** Profiling deliverable. Records the methodology, the measured numbers,
and the per-area dispositions for the M17 task:

> *Performance profiling pass on decomposition, chunk management, and rendering.*

This was a **measure-then-fix** pass. It added a reusable CPU profiler and a
headless benchmark harness, measured the three named subsystems under
representative workloads, applied the one optimization the data justified
(rendering: mesh-build scratch reuse), and recorded the dispositions for
everything else. Default engine behavior is byte-identical — the profiler is
disabled unless a benchmark/dev harness turns it on.

---

## 1. Instrumentation and harness (the lasting deliverables)

### Profiler (`include/core/Profiler.h`, `src/core/Profiler.cpp`)

A lightweight, always-compiled, **disabled-by-default** CPU zone profiler — the
same process-global shape as `engineConfig()`. A `VOXEL_PROFILE_SCOPE("name")`
RAII timer reads enablement once at construction; when off (the default) it never
reads the clock or touches the registry, so instrumented code is byte-identical in
behavior and effectively free in normal runs (one relaxed atomic load + a branch).
It is thread-safe (the decomposition worker threads may record concurrently), but
the lock is taken only while enabled, so the default path never contends. Covered
by `tests/ProfilerTest.cpp` (accumulation, reset, inert-when-disabled, thread
safety).

Zones placed this pass:

| Zone               | Area              | Site |
|--------------------|-------------------|------|
| `decomp.tick`      | Decomposition     | whole `DecompositionManager::tick` |
| `decomp.approach`  | Decomposition     | per-voxel approach-trigger candidate scan + enqueue |
| `decomp.stream`    | Chunk management  | per-layer composite load + evict + budget |
| `decomp.immutable` | Chunk management  | immutable-layer streaming pass |
| `stream.desired`   | Chunk management  | `StreamingVolume::desired` residency-set rebuild |
| `mesh.build`       | Rendering (CPU)   | `buildChunkMeshData` |

### Benchmark harness (`bench/profile_pass.cpp`, target `profile-pass`)

Headless — constructs no `Renderer` and opens no window, so it runs in CI and on a
build server. It drives:

1. **Decomposition + chunk management** — a 4-level composite cascade
   (`continental 64 m → regional 16 m → local 4 m → terrain 1 m`, the shape of
   demos 10/19) filled solid so every macro within the approach radius decomposes
   (the heaviest case for the approach scan and the streaming/eviction passes).
   Phase A settles the cascade around a fixed camera; phase B sweeps the camera to
   force load/evict and residency-set churn.
2. **Rendering (CPU mesher)** — meshes representative 32³ chunks (a solid floor, a
   half-filled wedge, and a 3-D checkerboard = the mesher's worst case) many times,
   under two output-buffer patterns (fresh-per-build vs reused scratch) so the
   measurement isolates allocation cost.

The GPU-submission half of rendering (draw-call cost, GPU time) is **not**
measurable headlessly — it needs a device. That half is already covered at runtime
by `EngineMetrics` (`drawCalls` from `bgfx::getStats()->numDraw`, `frameTimeSec`)
and was addressed structurally by the M17 view-frustum-culling task (only chunks
intersecting the frustum are submitted). This pass measures the **CPU** mesh-build
cost, which runs on the main thread and competes with the game loop.

Run it with `profile-pass [scale]` (the optional integer scales iteration counts).

---

## 2. Measured results

Release build, MSVC, Windows 11, one representative run (`scale=1`). Absolute
numbers are machine-specific; the **ratios and dispositions** are the takeaway.

### Decomposition + chunk management

```
decomposition settle: 150 ticks in 450.6 ms (3.004 ms/tick) — 956 terminal chunks generated
streaming churn:      150 ticks in 452.4 ms (3.016 ms/tick)

zone                   calls    total ms    avg us    max us
------------------------------------------------------------
decomp.tick              300      53.098   176.993  1633.600
decomp.approach          300      24.787    82.624   766.800
decomp.stream            900      18.383    20.425   799.500
stream.desired           300       0.461     1.536     6.600
decomp.immutable         300       0.021     0.070     0.400
```

(The ~3 ms/tick wall-clock includes a deliberate 2 ms/tick sleep modelling frame
pacing so the worker threads make progress; the profiler zones are the real CPU
cost.)

Reading:

- **A full decomposition tick costs ~0.18 ms on average, ~1.6 ms worst case** —
  comfortably inside a 16.6 ms (60 fps) frame even at its peak, and that peak is a
  cold-start burst, not steady state. The per-frame work budgets
  (`engineConfig()`, runtime-tunable since M17 D3b) cap throughput precisely so a
  tick can never turn a completion burst into a hitch.
- **The approach-trigger scan (`decomp.approach`) is the dominant decomposition
  phase** (~47% of tick time). It gathers candidate macros by scanning the voxels
  of resident composite chunks; it is already gated by a whole-chunk AABB
  pre-rejection (`chunkSurfaceDistSq`) and per-voxel cheapest-gate-first ordering,
  and bounded each tick by `decompPerFrame`. At the tested radii it is well within
  budget.
- **The residency-set rebuild (`stream.desired`) is negligible** (~1.5 µs/tick)
  for box volumes — the common case and the one all heightmap/slab worlds use.

### Rendering — CPU chunk mesher

```
--- fresh output buffers (pre-fix caller) ---
mesh solid-floor   2000 builds in 1842.2 ms (0.9211 ms/build)
mesh half-wedge    2000 builds in 2784.3 ms (1.3921 ms/build)
mesh checkerboard  2000 builds in 48011.1 ms (24.0056 ms/build)

--- reused output buffers (post-fix caller) ---
mesh solid-floor   2000 builds in 1390.0 ms (0.6950 ms/build)
mesh half-wedge    2000 builds in 1862.1 ms (0.9311 ms/build)
mesh checkerboard  2000 builds in 26081.2 ms (13.0406 ms/build)
```

Reading: the mesher emits **byte-identical** geometry in both columns — only the
output-buffer lifetime differs. Reusing the buffers across builds cuts CPU
mesh-build time by **~25% (light chunks) to ~46% (dense chunks)**. The cost it
removes is the `malloc`/`free` + geometric-growth reallocation that a fresh vector
set pays on every build.

---

## 3. Changes applied (justified by the measurement)

### Mesh-build scratch reuse — `ChunkMesh::build` (`src/renderer/ChunkMesh.cpp`)

`ChunkMesh::build` previously allocated a fresh `std::vector` set for the vertices
and the two index batches **on every chunk it meshed**. It now keeps `thread_local`
scratch vectors and passes those to `buildChunkMeshData` (which already `.clear()`s
them at entry, so the capacity grown by earlier builds is retained and steady-state
meshing never reallocates). The bgfx upload copies out of the scratch, so reusing
it is sound; `thread_local` keeps it correct if meshing is ever driven off more
than one thread. **Output is unchanged** — all 490 tests stay green, including the
`ChunkMeshData`/`VertexLayout` suites that pin the exact emitted geometry.

This is the headline win of the pass: a 25–46% reduction in the per-chunk CPU cost
of the operation that runs every time a chunk streams in, decomposes, or is edited.

### Mesher reserve — measured, rejected, **not** shipped

The obvious first guess — `reserve()`-ing the mesher's output vectors up front —
was implemented and measured. It was a **wash at best and a net loss at worst**: a
fixed size estimate either over-reserves sparse chunks (wasted memory) or
under-reserves dense ones (an extra allocation that is then reallocated away
anyway). The real cost was never the growth pattern within one build but the
**per-build allocation**, which scratch reuse removes entirely. The reserve was
reverted; a comment in `buildChunkMeshData` records why and points at the caller
fix. This is the pass doing its job — the speculative optimization lost to the
measured one.

---

## 4. Dispositions for everything else (measured, no change needed)

- **Decomposition tick budgets — adequate, no change.** Tick cost sits well inside
  a frame and the per-frame budgets prevent hitching by construction. No
  restructuring of the approach scan is justified at the tested scales; if a future
  world pushes far more resident composite voxels, the scan is the first place to
  revisit (a per-chunk "has any undecomposed solid" dirty flag would let it skip
  fully-decomposed chunks without scanning their voxels). Recorded, not actioned.

- **Residency-set rebuild (`StreamingVolume::desired`) — adequate, no change.** It
  rebuilds the full desired set and does O(volume) hash lookups every tick even at
  steady state, but at sub-microsecond cost for box volumes this is far below the
  noise floor. An incremental scheme (recompute only when the camera changes chunk,
  diff against the resident set) is possible but unjustified now. Recorded.

- **View-frustum culling (sanity-check A7) — already shipped.** The M17 task folded
  "measure first, implement if material" into this pass; culling shipped (engine
  `Frustum`, `BgfxRenderer::render` culls `pendingChunks` before sort + submit).
  The CPU cull is a cheap bounding-sphere test per chunk and reduces both the
  translucent sort cost and GPU submission. Validated as kept.

- **Memory backstop (sanity-check B4 / ARCHITECTURE §11) — remains deferred, with
  measured justification.** The operative cap on resident memory and on the GPU
  buffer-handle pool is the per-layer **`resident_chunk_budget`** (chunk-count
  budget), which the benchmark exercises and which bounds the terminal-chunk set
  (the bulk of the load). A second, **byte**-budget backstop adds value only if a
  future content type makes per-chunk byte size highly variable; until then the
  chunk-count budget is sufficient and the byte backstop stays parked where the gap
  audit left it. Recorded as a deliberate non-change.

---

## 5. Reproducing

```
cmake --build build --config Release --target profile-pass
./build/Release/profile-pass            # or profile-pass 4 to scale up the workload
```

To re-measure the mesh A/B against any future mesher change, the harness already
reports both the fresh-buffer and reused-buffer columns side by side. To profile a
new subsystem, wrap its hot path in `VOXEL_PROFILE_SCOPE("area.phase")`, enable the
profiler around the workload (`profiler().reset(); profiler().setEnabled(true);
...; profiler().setEnabled(false);`), and print `profiler().report()`.

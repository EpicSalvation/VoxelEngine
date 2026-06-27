// M17 performance profiling pass — headless benchmark harness.
//
// Drives the three subsystems the README task names — decomposition, chunk
// management, and rendering (the CPU mesher) — under representative workloads,
// with the engine's Profiler (include/core/Profiler.h) enabled, and prints the
// per-zone report plus wall-clock throughput for each section.
//
// It is deliberately headless: it constructs no Renderer and opens no window, so
// it runs in CI and on a build server. The GPU submission half of rendering
// cannot be measured here (it needs a device); this harness measures the CPU
// mesh-build cost, which is the part that runs on the main thread and competes
// with the game loop. The findings are recorded in docs/m17-performance-profiling.md.
//
// Build: configured as the `profile-pass` target (see CMakeLists.txt). Run with
// no arguments. Optional first argument scales the workload iteration counts.

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/Profiler.h"
#include "renderer/ChunkMeshData.h"
#include "world/Chunk.h"
#include "world/DecompositionManager.h"
#include "world/Layer.h"
#include "world/World.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ── Decomposition / chunk-management workload ───────────────────────────────────
//
// A 4-level composite cascade (continental→regional→local→terrain), the shape of
// demos 10/19, filled solid so every macro within the approach radius decomposes —
// the heaviest case for the per-tick approach scan and the streaming/eviction
// passes. Parent voxel == child chunk world size at every hop (coarse-supersets-
// fine), as the validator requires.
const char* kCascadeYaml = R"(
layers:
  - name: continental
    voxel_size_m: 64.0
    mode: composite
    decompose_to: regional
    chunk_size_voxels: 4
    view_distance_chunks: 2
    decompose_distance_m: 220.0
  - name: regional
    voxel_size_m: 16.0
    mode: composite
    decompose_to: local
    chunk_size_voxels: 4
    view_distance_chunks: 2
    decompose_distance_m: 90.0
  - name: local
    voxel_size_m: 4.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 4
    view_distance_chunks: 2
    decompose_distance_m: 28.0
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 4
    view_distance_chunks: 3
)";

// Solid generator for every layer. Zero-initializes padding (the determinism
// rule the test generators follow) and fills every voxel solid.
void solidGen(WorldCoord /*origin*/, int n, Voxel* out, void*) {
    Voxel v{};
    v.material.palette_index = 1;
    v.material.density       = 100.0f;
    v.material.hardness      = 1.0f;
    for (int i = 0; i < n * n * n; ++i) out[i] = v;
}

int solidPluginInit(PluginContext* ctx) {
    for (const char* name : {"continental", "regional", "local", "terrain"})
        ctx->register_layer_generator(ctx, name, solidGen, nullptr);
    return 0;
}

void benchDecomposition(int ticks) {
    auto cfg = LayerConfig::loadFromString(kCascadeYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xC0FFEEull, /*threads=*/2);

    const double approach = 64.0;  // fallback; per-layer radii drive the cascade

    // Phase A: settle the cascade around a fixed camera (decompose-on-approach).
    WorldCoord cam(0.0, 0.0, 0.0);
    const auto t0 = Clock::now();
    for (int i = 0; i < ticks; ++i) {
        mgr.tick(cam, approach);
        // Let the worker threads make progress so completed jobs are applied and
        // the next level is enqueued — mirrors a real frame's pacing.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double settleMs = msSince(t0);
    const size_t settledTerminal =
        world.layer("terrain") ? world.layer("terrain")->chunks().size() : 0u;

    // Phase B: streaming churn — sweep the camera so chunks load and evict and the
    // residency set (StreamingVolume::desired) is rebuilt every tick.
    const auto t1 = Clock::now();
    for (int i = 0; i < ticks; ++i) {
        cam.value.x += 12.0;  // ~chunk-scale steps at the finer layers
        mgr.tick(cam, approach);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double churnMs = msSince(t1);

    std::printf("  decomposition settle: %d ticks in %.1f ms (%.3f ms/tick)"
                " — %zu terminal chunks generated\n",
                ticks, settleMs, settleMs / ticks, settledTerminal);
    std::printf("  streaming churn:      %d ticks in %.1f ms (%.3f ms/tick)\n",
                ticks, churnMs, churnMs / ticks);
}

// ── Rendering (CPU mesher) workload ─────────────────────────────────────────────
//
// Build meshes for a set of representative 32^3 chunks many times. Three fill
// patterns bracket the real range: a solid floor (light — one exposed surface), a
// half-full block (medium), and a dense 3D checkerboard (heavy — maximal exposed
// face count, the mesher's worst case).
Chunk makeChunk(int n, int pattern) {
    Chunk c({0, 0, 0}, n, WorldCoord(0.0, 0.0, 0.0));
    Voxel solid{};
    solid.material.palette_index = 1;
    solid.material.density       = 100.0f;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                bool fill = false;
                switch (pattern) {
                    case 0: fill = (y < n / 2); break;                 // solid floor
                    case 1: fill = (x + y + z) < (3 * n / 2); break;   // half wedge
                    default: fill = ((x ^ y ^ z) & 1) != 0; break;     // checkerboard
                }
                if (fill) c.at(x, y, z) = solid;
            }
    return c;
}

void benchMeshing(int iterations) {
    const int n = 32;
    const Chunk chunks[3] = {makeChunk(n, 0), makeChunk(n, 1), makeChunk(n, 2)};
    const char* names[3]  = {"solid-floor", "half-wedge", "checkerboard"};

    // Two allocation patterns, the A/B for the ChunkMesh::build scratch-reuse fix:
    //   fresh  — a brand-new output vector set per build (the pre-fix caller)
    //   reused — one scratch set kept across builds (the post-fix caller; .clear()
    //            retains capacity so steady-state builds never reallocate)
    // The mesher emits byte-identical geometry either way; only the buffer
    // lifetime differs, so this isolates the allocation cost the fix removes.
    for (int mode = 0; mode < 2; ++mode) {
        const bool reused = (mode == 1);
        std::printf("  --- %s output buffers ---\n", reused ? "reused" : "fresh");
        std::vector<MeshVertex> sVerts;
        std::vector<uint32_t>   sOpaque, sTransl;
        for (int p = 0; p < 3; ++p) {
            const auto t0 = Clock::now();
            size_t lastVerts = 0;
            for (int i = 0; i < iterations; ++i) {
                if (reused) {
                    buildChunkMeshData(chunks[p], sVerts, sOpaque, sTransl);
                    lastVerts = sVerts.size();
                } else {
                    std::vector<MeshVertex> verts;
                    std::vector<uint32_t>   opaqueIdx, translucentIdx;
                    buildChunkMeshData(chunks[p], verts, opaqueIdx, translucentIdx);
                    lastVerts = verts.size();
                }
            }
            const double ms = msSince(t0);
            std::printf("  mesh %-13s %d builds in %.1f ms (%.4f ms/build, %zu verts)\n",
                        names[p], iterations, ms, ms / iterations, lastVerts);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    int scale = 1;
    if (argc > 1) scale = std::max(1, std::atoi(argv[1]));

    const int decompTicks    = 150 * scale;
    const int meshIterations = 2000 * scale;

    std::printf("=== M17 performance profiling pass (scale=%d) ===\n\n", scale);

    // ── Decomposition + chunk management ──
    std::printf("[1] Decomposition + chunk management\n");
    profiler().reset();
    profiler().setEnabled(true);
    benchDecomposition(decompTicks);
    profiler().setEnabled(false);
    std::printf("\n%s\n", profiler().report().c_str());

    // ── Rendering (CPU mesher) ──
    std::printf("[2] Rendering — CPU chunk mesher\n");
    profiler().reset();
    profiler().setEnabled(true);
    benchMeshing(meshIterations);
    profiler().setEnabled(false);
    std::printf("\n%s\n", profiler().report().c_str());

    return 0;
}

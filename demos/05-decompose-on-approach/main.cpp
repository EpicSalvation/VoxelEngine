// M6 demo — decompose on approach.
//
// A three-layer world (see the layered-world plugin) exercises the multi-layer
// scale system:
//
//   - "blocks"   — a composite layer of coarse 8 m macro voxels, a blocky
//                  stand-in for distant terrain. Rendered as atomic blocks.
//   - "terrain"  — the fine 1 m terminal layer the macro voxels decompose into.
//                  Terrain chunks exist only where a block has decomposed.
//   - "backdrop" — an immutable 2 m bedrock slab beneath the terrain that
//                  renders and collides but is never edited, persisted, or
//                  decomposed.
//
// What it teaches (and where it sits on the ladder): this is the first rung of
// the cascade story, so it leans entirely on the engine-owned facilities rather
// than hand-rolling them. The M10 DecompositionManager owns the whole
// approach → drain → evict pipeline for the composite "blocks" layer AND streams
// the immutable "backdrop" under its own StreamingVolume + resident_chunk_budget
// (M16 L5) — the demo just applies the per-tick diff to its mesh stores. Flying
// toward the blocky horizon refines it into detailed terrain (with brief pop-in
// while a decomposition job is in flight — expected, not a bug; ARCHITECTURE §4),
// and flying away cascade-collapses the fine terrain back to coarse blocks so the
// resident mesh count stays bounded under bgfx's static-buffer ceiling. Demo 10
// (drill-to-the-core) is the deep-stack version of this same pattern.
//
// Controls: WASD move, mouse look, Space/Shift up/down (fly) or jump (walk),
// G toggles walk (gravity + swept AABB collision across all layers), F toggles
// the cursor, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionManager.h"
#include "world/VoxelCollision.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VOXEL_LAYERED_PLUGIN_PATH
#  define VOXEL_LAYERED_PLUGIN_PATH ""
#endif

namespace {
constexpr char   kLogCat[] = "demo05";

constexpr uint64_t kWorldSeed = 0x05DEC0FFEE05ull;

// Decompose a composite macro voxel when the camera is within this range of it.
// The manager streams the "blocks" root layer out to its view distance (256 m)
// and decomposes only inside this bubble; the per-layer resident_chunk_budget
// collapses the farthest decomposed terrain back to coarse blocks so the fine
// mesh count stays well under bgfx's 4096 static-buffer ceiling (ARCHITECTURE §10).
constexpr double kApproachRadiusM = 144.0;

constexpr float  kFlySpeed  = 32.0f;
constexpr float  kMouseSens = 0.002f;

constexpr double kWalkSpeed = 6.0;
constexpr double kGravity   = 25.0;
constexpr double kJumpSpeed = 8.0;
constexpr double kEyeOffset = 0.7;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// Replace (or create) the mesh for a chunk, destroying the old GPU buffers first
// (plain map assignment leaks bgfx handles into the capped static-buffer pool).
// All-air chunks produce no faces and are kept OUT of the store entirely.
void rebuildMesh(MeshStore& ms, const ChunkCoord& cc, const Chunk& chunk) {
    auto it = ms.find(cc);
    if (it != ms.end()) { it->second.destroy(); ms.erase(it); }
    ChunkMesh mesh = ChunkMesh::build(chunk);
    if (mesh.empty()) return;
    ms.emplace(cc, std::move(mesh));
}

// Apply one DecompositionManager::tick() diff set to the per-layer mesh stores.
// This is the M10 front-end contract (mirrors demo 10's applyDiffs): the manager
// owns all decomposition/streaming state and only tells us which meshes to build
// or destroy. It handles immutable-layer diffs too — those arrive with the layer
// name in compositeLayerName and their load/evict in newCompChunks/evictedCompChunks.
void applyDiffs(const std::vector<LayerTickDiff>& diffs, World& world,
                std::unordered_map<std::string, MeshStore>& meshStores) {
    for (const LayerTickDiff& d : diffs) {
        MeshStore& compMeshes  = meshStores[d.compositeLayerName];
        Layer* compLayer  = world.layer(d.compositeLayerName);
        Layer* childLayer = d.childLayerName.empty() ? nullptr
                                                     : world.layer(d.childLayerName);

        // Composite (or immutable) chunks loaded/evicted this tick.
        for (const ChunkCoord& cc : d.newCompChunks) {
            if (!compLayer) continue;
            if (const Chunk* chunk = compLayer->getChunk(cc)) rebuildMesh(compMeshes, cc, *chunk);
        }
        for (const ChunkCoord& cc : d.evictedCompChunks) {
            auto it = compMeshes.find(cc);
            if (it != compMeshes.end()) { it->second.destroy(); compMeshes.erase(it); }
        }

        // Macro voxels whose block state changed: remesh the owning composite chunk
        // (newly decomposed macros had their block cleared; re-atomized macros had
        // it restored). Top-down eviction already dropped the chunk → getChunk null
        // → skipped. One terrain chunk per macro here, so no dedupe needed, but the
        // owning-chunk lookup still collapses many macros that share a chunk.
        if (compLayer) {
            for (const chunkmath::VoxelCoord& macro : d.newlyDecomposed) {
                const ChunkCoord cc =
                    chunkmath::voxelToChunkLocal(macro, compLayer->chunkSizeVoxels()).chunk;
                if (const Chunk* chunk = compLayer->getChunk(cc)) rebuildMesh(compMeshes, cc, *chunk);
            }
            for (const chunkmath::VoxelCoord& macro : d.newlyAtomic) {
                const ChunkCoord cc =
                    chunkmath::voxelToChunkLocal(macro, compLayer->chunkSizeVoxels()).chunk;
                if (const Chunk* chunk = compLayer->getChunk(cc)) rebuildMesh(compMeshes, cc, *chunk);
            }
        }

        // Child (terrain) chunks produced/removed by decomposition.
        if (childLayer) {
            MeshStore& childMeshes = meshStores[d.childLayerName];
            for (const ChunkCoord& cc : d.newChildChunks)
                if (const Chunk* chunk = childLayer->getChunk(cc)) rebuildMesh(childMeshes, cc, *chunk);
            for (const ChunkCoord& cc : d.evictedChildChunks) {
                auto it = childMeshes.find(cc);
                if (it != childMeshes.end()) { it->second.destroy(); childMeshes.erase(it); }
            }
        }
    }
}

}  // namespace

int main() {
    Log::setMinLevel(Log::Level::Info);

    // ── Layer config ────────────────────────────────────────────────────────────
    // Coarsest-first (the validation order). blocks(8 m)→backdrop(2 m)→terrain(1 m)
    // satisfies the adjacent strictly-descending integer-ratio rule (8/2=4, 2/1=2).
    // Parent voxel (8 m) == terrain chunk world size (1 m × 8) so each macro voxel
    // decomposes into exactly one terrain chunk. Per-layer resident_chunk_budget
    // bounds the resident mesh count (M16 L5); the terrain cap matters most — fine
    // chunks dominate the bgfx static-buffer load.
    LayerConfig layerConfig = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 8
    view_distance_chunks: 4
    resident_chunk_budget: 256
  - name: backdrop
    voxel_size_m: 2.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 6
    resident_chunk_budget: 512
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    view_distance_chunks: 6
    resident_chunk_budget: 2048
)");
        } catch (const std::exception& e) {
            Log::error(kLogCat, (std::string("Fatal: layer config error: ") + e.what()).c_str());
            std::exit(1);
        }
    }();

    // ── Plugin loading ────────────────────────────────────────────────────────
    // Materials and the per-layer generators come from the layered-world plugin.
    if (std::string(VOXEL_LAYERED_PLUGIN_PATH).empty()) {
        Log::error(kLogCat, "Fatal: layered-world plugin path not configured at build time.");
        return 1;
    }

    World world(layerConfig);
    Engine engine;
    PluginManager pluginManager;
    engine.init(pluginManager, world);

    if (pluginManager.loadPlugin(VOXEL_LAYERED_PLUGIN_PATH) == kInvalidPluginId) {
        Log::error(kLogCat, (std::string("Fatal: could not load layered-world plugin from ")
                             + VOXEL_LAYERED_PLUGIN_PATH).c_str());
        return 1;
    }

    Layer* blocks   = world.layer("blocks");
    Layer* backdrop = world.layer("backdrop");
    Layer* terrain  = world.layer("terrain");
    if (!blocks || !backdrop || !terrain) {
        Log::error(kLogCat, "Fatal: expected blocks/backdrop/terrain layers.");
        return 1;
    }

    // ── Engine-owned cascade orchestrator ───────────────────────────────────────
    // Owns "blocks" decomposition AND "backdrop" immutable streaming. The
    // composite surface lives in blocks chunk-Y 0 and the bedrock slab in
    // backdrop chunk-Y -1; band the generator-streamed root to its row so empty
    // sky/underground chunks are never loaded (the backdrop, an immutable layer,
    // is streamed by the manager under its own volume independently).
    DecompositionManager decompMgr(world, pluginManager, layerConfig, kWorldSeed);
    decompMgr.setVerticalBand(0, 0);

    platform::Window window(1024, 768, "VoxelEngine — M6 Decompose on Approach");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);
    engine.setRenderer(&renderer);
    engine.setDecompositionManager(&decompMgr);

    // One MeshStore per layer (blocks composite, backdrop immutable, terrain terminal).
    std::unordered_map<std::string, MeshStore> meshStores;

    // Camera.
    float      pitch = -0.35f, yaw = 0.0f;
    WorldCoord camPos(0.0, 40.0, -48.0);
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true;
    bool       cursorCaptured = true;
    bool       prevKeyF = false, prevKeyG = false;

    bool       walkMode = false;
    WorldCoord playerCenter(0.0, 0.0, 0.0);
    double     vy = 0.0;
    bool       grounded = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    Log::info(kLogCat, "Fly toward the blocky terrain to decompose it. WASD + mouse, "
                       "Space/Shift up/down, G = walk (collision), F = cursor, ESC quits.");

    while (!window.shouldClose()) {
        window.pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        bool curKeyF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curKeyF && !prevKeyF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                             cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevKeyF = curKeyF;

        bool curKeyG = (glfwGetKey(glfwWin, GLFW_KEY_G) == GLFW_PRESS);
        if (curKeyG && !prevKeyG) {
            walkMode = !walkMode;
            if (walkMode) {
                playerCenter = WorldCoord(camPos.value - glm::dvec3(0.0, kEyeOffset, 0.0));
                vy = 0.0;
                grounded = false;
            }
            Log::info(kLogCat, walkMode ? "Mode: WALK (gravity + collision)" : "Mode: FLY");
        }
        prevKeyG = curKeyG;

        if (cursorCaptured) {
            double mx, my;
            glfwGetCursorPos(glfwWin, &mx, &my);
            if (!firstMouse) {
                yaw   += static_cast<float>(mx - lastMouseX) * kMouseSens;
                pitch -= static_cast<float>(my - lastMouseY) * kMouseSens;
                if (pitch >  1.55f) pitch =  1.55f;
                if (pitch < -1.55f) pitch = -1.55f;
            }
            lastMouseX = mx;
            lastMouseY = my;
            firstMouse = false;
        }

        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);

        if (!walkMode) {
            glm::dvec3 fwd  {static_cast<double>(cp * sy), static_cast<double>(sp),
                             static_cast<double>(cp * cy)};
            glm::dvec3 right{static_cast<double>(cy), 0.0, static_cast<double>(-sy)};
            glm::dvec3 delta{0.0, 0.0, 0.0};
            double step = static_cast<double>(kFlySpeed * dt);
            if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += fwd   * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= fwd   * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) delta -= right * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) delta += right * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) delta.y += step;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) delta.y -= step;
            camPos = WorldCoord(camPos.value + delta);
        } else {
            glm::dvec3 fwdH  {static_cast<double>(sy), 0.0, static_cast<double>(cy)};
            glm::dvec3 rightH{static_cast<double>(cy), 0.0, static_cast<double>(-sy)};
            glm::dvec3 wish{0.0, 0.0, 0.0};
            if (glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS) wish += fwdH;
            if (glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS) wish -= fwdH;
            if (glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS) wish -= rightH;
            if (glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS) wish += rightH;
            if (glm::length(wish) > 0.0) wish = glm::normalize(wish);

            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE) == GLFW_PRESS && grounded)
                vy = kJumpSpeed;
            vy -= kGravity * static_cast<double>(dt);

            glm::dvec3 delta = wish * (kWalkSpeed * static_cast<double>(dt));
            delta.y += vy * static_cast<double>(dt);

            voxelcollide::MoveResult mr =
                voxelcollide::moveAABB(world, {playerCenter, kPlayerHalf}, delta);
            playerCenter = mr.position;
            grounded = mr.grounded;
            if (mr.grounded || (mr.hitY && vy > 0.0)) vy = 0.0;
            camPos = WorldCoord(playerCenter.value + glm::dvec3(0.0, kEyeOffset, 0.0));
        }

        // ── DecompositionManager tick ───────────────────────────────────────────
        // Streams the "blocks" root and the "backdrop" immutable layer, runs
        // decomposition inside the approach bubble, and cascade-collapses chunks
        // that drift out of range / over budget. The returned diffs say which
        // meshes to build or destroy; applyPerFrame bounds per-frame mesh builds.
        auto diffs = decompMgr.tick(camPos, kApproachRadiusM,
                                    /*loadPerFrame=*/32, /*decompPerFrame=*/128,
                                    /*applyPerFrame=*/16);
        applyDiffs(diffs, world, meshStores);

        // Terminal terrain is populated entirely by decomposition; guard against
        // any mesh whose chunk the cascade has since removed from the store.
        {
            MeshStore& terrainMeshes = meshStores["terrain"];
            std::vector<ChunkCoord> stale;
            for (const auto& kv : terrainMeshes)
                if (!terrain->getChunk(kv.first)) stale.push_back(kv.first);
            for (const ChunkCoord& cc : stale) {
                terrainMeshes[cc].destroy();
                terrainMeshes.erase(cc);
            }
        }

        // Resize.
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        // ── Render every layer at its own voxel scale (coarsest first) ───────────
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        for (const auto& layerName : std::vector<std::string>{"backdrop", "blocks", "terrain"}) {
            Layer* lyr = world.layer(layerName);
            if (!lyr) continue;
            const MeshStore& ms = meshStores[layerName];
            for (const auto& kv : ms) {
                const Chunk* chunk = lyr->getChunk(kv.first);
                if (!chunk || kv.second.empty()) continue;
                renderer.renderChunk(kv.second, chunk->origin(),
                                     lyr->voxelSizeM(), lyr->chunkSizeVoxels());
            }
        }
        renderer.render();
    }

    for (auto& [name, ms] : meshStores)
        for (auto& [cc, mesh] : ms)
            mesh.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

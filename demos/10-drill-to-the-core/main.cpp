// M10 demo — drill to the core.
//
// A four-layer composite chain (continental → regional → local → terrain)
// driven by the engine-owned DecompositionManager (the M10 cascade orchestrator).
// Fly toward a continental block to decompose it into regional blocks; approach
// one of those to reveal local blocks; approach local blocks to see the fine 1 m
// terrain carved by caves and topped with soil.
//
// The HUD shows per-layer resident-chunk counts and decomposed macro counts, and
// in-flight worker jobs. Fly far away and the region cascade-evicts back to a
// single coarse block; revisit to confirm it regenerates identically (determinism).
//
// Controls: WASD move, mouse look, Space/Shift up/down, G walk mode,
//           F cursor, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/RecipeValidation.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionManager.h"
#include "world/VoxelCollision.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VOXEL_DRILL_PLUGIN_PATH
#  define VOXEL_DRILL_PLUGIN_PATH ""
#endif

namespace {

constexpr double kApproachRadiusM = 640.0;  // decompose when within this range
constexpr float  kFlySpeed        = 120.0f;
constexpr float  kMouseSens       = 0.002f;
constexpr double kWalkSpeed       = 6.0;
constexpr double kGravity         = 25.0;
constexpr double kJumpSpeed       = 8.0;
constexpr double kEyeOffset       = 0.7;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

constexpr uint64_t kWorldSeed = 0xDECAFBAD12345678ull;

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// Apply the diffs from DecompositionManager::tick() to the per-layer mesh stores.
// For each composite layer: build meshes for new chunks, destroy evicted meshes.
// For each child layer:      build meshes for new child chunks, destroy evicted.
void applyDiffs(const std::vector<LayerTickDiff>& diffs, World& world,
                std::unordered_map<std::string, MeshStore>& meshStores) {
    for (const LayerTickDiff& d : diffs) {
        MeshStore& compMeshes  = meshStores[d.compositeLayerName];
        MeshStore& childMeshes = meshStores[d.childLayerName];
        Layer* compLayer  = world.layer(d.compositeLayerName);
        Layer* childLayer = world.layer(d.childLayerName);

        // New composite chunks (build mesh for the coarse block representation).
        for (const ChunkCoord& cc : d.newCompChunks) {
            if (!compLayer) continue;
            const Chunk* chunk = compLayer->getChunk(cc);
            if (chunk) compMeshes[cc] = ChunkMesh::build(*chunk);
        }
        // Evicted composite chunks (destroy mesh).
        for (const ChunkCoord& cc : d.evictedCompChunks) {
            auto it = compMeshes.find(cc);
            if (it != compMeshes.end()) { it->second.destroy(); compMeshes.erase(it); }
        }

        // Macro voxels just decomposed: remesh the composite chunk they live in
        // (the block voxel was cleared — the mesh needs updating).
        for (const chunkmath::VoxelCoord& macro : d.newlyDecomposed) {
            if (!compLayer) continue;
            const chunkmath::LocalVoxel lv =
                chunkmath::voxelToChunkLocal(macro, compLayer->chunkSizeVoxels());
            const Chunk* chunk = compLayer->getChunk(lv.chunk);
            if (chunk) compMeshes[lv.chunk] = ChunkMesh::build(*chunk);
        }

        // New child chunks (build fine mesh).
        for (const ChunkCoord& cc : d.newChildChunks) {
            if (!childLayer) continue;
            const Chunk* chunk = childLayer->getChunk(cc);
            if (chunk) childMeshes[cc] = ChunkMesh::build(*chunk);
        }
        // Evicted child chunks (destroy fine mesh).
        for (const ChunkCoord& cc : d.evictedChildChunks) {
            auto it = childMeshes.find(cc);
            if (it != childMeshes.end()) { it->second.destroy(); childMeshes.erase(it); }
        }
    }
}

}  // namespace

int main() {
    // ── Layer config ──────────────────────────────────────────────────────────
    // Parent voxel size == child chunk world size at every level, guaranteeing
    // the coarse-supersets-fine invariant (ARCHITECTURE §4, M10):
    //   continental 512 m, chunk 8 → 512 m/chunk  (ratio 8 to regional)
    //   regional     64 m, chunk 8 →  64 m/chunk  (ratio 8 to local)
    //   local         8 m, chunk 8 →   8 m/chunk  (ratio 8 to terrain)
    //   terrain       1 m, chunk 8 →   8 m/chunk  (terminal)
    LayerConfig cfg = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: continental
    voxel_size_m: 512.0
    mode: composite
    decompose_to: regional
    chunk_size_voxels: 8
    view_distance_chunks: 2

  - name: regional
    voxel_size_m: 64.0
    mode: composite
    decompose_to: local
    chunk_size_voxels: 8
    view_distance_chunks: 3

  - name: local
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 8
    view_distance_chunks: 4

  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    view_distance_chunks: 8
)");
        } catch (const std::exception& e) {
            std::cerr << "[main] Fatal: " << e.what() << "\n";
            std::exit(1);
        }
    }();

    // ── Plugin loading ────────────────────────────────────────────────────────
    if (std::string(VOXEL_DRILL_PLUGIN_PATH).empty()) {
        std::cerr << "[main] Fatal: drill-world plugin not configured at build time.\n";
        return 1;
    }

    World world(cfg);
    Engine engine;
    PluginManager pm;
    engine.init(pm, world);

    if (pm.loadPlugin(VOXEL_DRILL_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load drill-world plugin from "
                  << VOXEL_DRILL_PLUGIN_PATH << "\n";
        return 1;
    }
    try {
        validateRecipes(cfg, pm);
    } catch (const std::exception& e) {
        std::cerr << "[main] Fatal: recipe validation: " << e.what() << "\n";
        return 1;
    }

    // ── Engine-owned cascade orchestrator ─────────────────────────────────────
    DecompositionManager decompMgr(world, pm, cfg, kWorldSeed);

    // ── Renderer ──────────────────────────────────────────────────────────────
    platform::Window window(1280, 720, "VoxelEngine — M10 Drill to the Core");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    // One MeshStore per layer (all four, plus terminal).
    // Keys are layer names; each MeshStore maps ChunkCoord → ChunkMesh.
    std::unordered_map<std::string, MeshStore> meshStores;

    // Terminal terrain layer: streamed by the demo independently of the manager.
    Layer* terrain = world.layer("terrain");
    if (!terrain) {
        std::cerr << "[main] Fatal: 'terrain' layer not found.\n";
        return 1;
    }
    const RegisteredLayerGenerator* terrainGenRec = nullptr;
    for (const auto& g : pm.layerGenerators())
        if (g.layer_name == "terrain") { terrainGenRec = &g; break; }

    // ── Camera / player ───────────────────────────────────────────────────────
    float      pitch = -0.25f, yaw = 0.0f;
    WorldCoord camPos(256.0, 1800.0, -512.0);  // start above the continental slab
    double     lastMX = 0, lastMY = 0;
    bool       firstMouse = true, cursorCaptured = true;
    bool       prevF = false, prevG = false;
    bool       walkMode = false;
    WorldCoord playerCenter(256.0, 1800.0, -512.0);
    double     vy = 0.0;
    bool       grounded = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    std::cout << "[main] Controls: WASD fly, Space/Shift up/down, G walk, F cursor, ESC quit.\n"
              << "[main] Fly toward the gray continental blocks to begin the cascade.\n";

    // ── Main loop ─────────────────────────────────────────────────────────────
    while (!window.shouldClose()) {
        auto now = std::chrono::high_resolution_clock::now();
        const float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;

        window.pollEvents();
        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // ── Mouse look ────────────────────────────────────────────────────────
        if (cursorCaptured) {
            double mx, my;
            glfwGetCursorPos(glfwWin, &mx, &my);
            if (firstMouse) { lastMX = mx; lastMY = my; firstMouse = false; }
            yaw   += static_cast<float>((mx - lastMX) * kMouseSens);
            pitch -= static_cast<float>((my - lastMY) * kMouseSens);
            pitch  = std::max(-1.5f, std::min(1.5f, pitch));
            lastMX = mx; lastMY = my;
        }

        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw),   sy = std::sin(yaw);
        const glm::dvec3 look(cp*sy, sp, cp*cy);
        const glm::dvec3 right(cy, 0, -sy);

        // ── F: toggle cursor ──────────────────────────────────────────────────
        const bool curF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curF && !prevF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevF = curF;

        // ── G: toggle walk mode ───────────────────────────────────────────────
        const bool curG = (glfwGetKey(glfwWin, GLFW_KEY_G) == GLFW_PRESS);
        if (curG && !prevG) {
            walkMode = !walkMode;
            if (walkMode) { playerCenter = camPos; vy = 0.0; }
        }
        prevG = curG;

        // ── Movement ──────────────────────────────────────────────────────────
        if (!walkMode) {
            glm::dvec3 wish(0);
            if (glfwGetKey(glfwWin, GLFW_KEY_W)     == GLFW_PRESS) wish += look;
            if (glfwGetKey(glfwWin, GLFW_KEY_S)     == GLFW_PRESS) wish -= look;
            if (glfwGetKey(glfwWin, GLFW_KEY_A)     == GLFW_PRESS) wish -= glm::dvec3(right);
            if (glfwGetKey(glfwWin, GLFW_KEY_D)     == GLFW_PRESS) wish += glm::dvec3(right);
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE) == GLFW_PRESS) wish.y += 1.0;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) wish.y -= 1.0;
            if (glm::length(wish) > 0.0) wish = glm::normalize(wish);
            camPos = WorldCoord(camPos.value + wish * (kFlySpeed * static_cast<double>(dt)));
        } else {
            glm::dvec3 fwdH(look.x, 0, look.z);
            if (glm::length(fwdH) > 0.0) fwdH = glm::normalize(fwdH);
            glm::dvec3 wish(0);
            if (glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS) wish += fwdH;
            if (glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS) wish -= fwdH;
            if (glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS) wish -= glm::dvec3(right.x,0,right.z);
            if (glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS) wish += glm::dvec3(right.x,0,right.z);
            if (glm::length(wish) > 0.0) wish = glm::normalize(wish);
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE) == GLFW_PRESS && grounded) vy = kJumpSpeed;
            vy -= kGravity * static_cast<double>(dt);
            glm::dvec3 delta = wish * (kWalkSpeed * static_cast<double>(dt));
            delta.y += vy * static_cast<double>(dt);
            auto mr = voxelcollide::moveAABB(world, {playerCenter, kPlayerHalf}, delta);
            playerCenter = mr.position;
            grounded = mr.grounded;
            if (mr.grounded || (mr.hitY && vy > 0.0)) vy = 0.0;
            camPos = WorldCoord(playerCenter.value + glm::dvec3(0, kEyeOffset, 0));
        }

        // ── DecompositionManager tick ─────────────────────────────────────────
        // The manager streams all composite layers, runs decomposition, and cascade-
        // evicts chunks that leave view range. The returned diffs tell us which
        // meshes to build or destroy.
        auto diffs = decompMgr.tick(camPos, kApproachRadiusM,
                                    /*loadPerFrame=*/4, /*decompPerFrame=*/64);
        applyDiffs(diffs, world, meshStores);

        // ── Terminal terrain: stream independently ────────────────────────────
        {
            MeshStore& terrainMeshes = meshStores["terrain"];
            // Streaming driven by the decomposition manager's child chunks for "local"
            // (they appear in the terrain layer via insertChunk). The terminal layer
            // itself does not need its own load loop since the manager inserts chunks
            // from decomposition. However, we do need to evict terrain chunks that are
            // outside view range (the manager evicts them via cascade, but for any
            // direct terminal chunks not produced by decomposition we handle here).
            // In this demo the terrain layer is 100% populated by decomposition.
            // Guard: destroy meshes for terrain chunks no longer in the ChunkStore.
            std::vector<ChunkCoord> stale;
            for (const auto& kv : terrainMeshes)
                if (!terrain->getChunk(kv.first)) stale.push_back(kv.first);
            for (const ChunkCoord& cc : stale) {
                terrainMeshes[cc].destroy();
                terrainMeshes.erase(cc);
            }
        }

        // ── HUD ───────────────────────────────────────────────────────────────
        {
            const int contDecomp = static_cast<int>(decompMgr.decomposedCount("continental"));
            const int regDecomp  = static_cast<int>(decompMgr.decomposedCount("regional"));
            const int locDecomp  = static_cast<int>(decompMgr.decomposedCount("local"));
            const int inFlight   = static_cast<int>(decompMgr.inFlight());
            const int terrChunks = static_cast<int>(terrain->chunks().size());
            char hud[256];
            std::snprintf(hud, sizeof(hud),
                "Decomposed: cont=%d  reg=%d  loc=%d | terrain chunks=%d | in-flight=%d",
                contDecomp, regDecomp, locDecomp, terrChunks, inFlight);
            renderer.setHudText({std::string(hud)});
        }

        // ── Render ────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        // Render all layers in reverse order (coarsest first, so finer voxels
        // occlude coarser ones via depth test).
        for (const auto& layerName : std::vector<std::string>{
                "continental", "regional", "local", "terrain"}) {
            Layer* lyr = world.layer(layerName);
            if (!lyr) continue;
            const MeshStore& ms = meshStores[layerName];
            for (const auto& kv : ms) {
                const Chunk* chunk = lyr->getChunk(kv.first);
                if (chunk && !kv.second.empty())
                    renderer.renderChunk(kv.second, chunk->origin(), lyr->voxelSizeM());
            }
        }

        renderer.render();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    for (auto& [name, ms] : meshStores)
        for (auto& [cc, mesh] : ms)
            mesh.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

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
#include "world/VoxelRaycast.h"
#include "world/World.h"
#include "simulation/RemovalAccumulator.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef VOXEL_DRILL_PLUGIN_PATH
#  define VOXEL_DRILL_PLUGIN_PATH ""
#endif

namespace {

constexpr double kApproachRadiusM = 64.0;   // decompose when within this range
constexpr double kReachM          = 12.0;   // voxel-pick reach in metres
constexpr float  kToolPower       = 5.0f;   // dig speed (units/s)
constexpr float  kFlySpeed        = 120.0f;
constexpr float  kMouseSens       = 0.002f;
constexpr double kWalkSpeed       = 6.0;
constexpr double kGravity         = 25.0;
constexpr double kJumpSpeed       = 8.0;
constexpr double kEyeOffset       = 0.7;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

constexpr uint64_t kWorldSeed = 0xDECAFBAD12345678ull;

// Must match the renderer: BgfxRenderer::render uses a 60° vertical FOV, and
// the far clip is whatever setFarClip received (see the setFarClip call below).
constexpr float  kFarClipM = 4000.0f;
constexpr double kVFovDeg  = 60.0;

// Conservative view-frustum test for chunk bounding spheres, built from the
// same camera basis and projection parameters the renderer uses. Plane-distance
// tests against the bounding sphere err on the side of "visible" — this never
// culls geometry the renderer would actually draw, it only skips submitting
// chunks behind the camera or outside the view cone.
struct Frustum {
    glm::dvec3 pos{}, fwd{}, right{}, up{};
    double tanH = 0.0, tanV = 0.0, cosH = 1.0, cosV = 1.0, farClip = 0.0;

    void update(const WorldCoord& camPos, float pitch, float yaw,
                double aspect, double vfovDeg, double farClipM) {
        pos = camPos.value;
        const double cp = std::cos(pitch), sp = std::sin(pitch);
        const double cy = std::cos(yaw),   sy = std::sin(yaw);
        fwd   = glm::dvec3(cp * sy, sp, cp * cy);  // matches BgfxRenderer::render
        right = glm::dvec3(cy, 0.0, -sy);
        up    = glm::cross(fwd, right);
        tanV  = std::tan(glm::radians(vfovDeg) * 0.5);
        tanH  = tanV * aspect;
        cosV  = 1.0 / std::sqrt(1.0 + tanV * tanV);
        cosH  = 1.0 / std::sqrt(1.0 + tanH * tanH);
        farClip = farClipM;
    }

    bool sphereVisible(const glm::dvec3& center, double radius) const {
        const glm::dvec3 rel = center - pos;
        const double z = glm::dot(rel, fwd);
        if (z + radius < 0.0 || z - radius > farClip) return false;
        // Side planes pass through the camera; signed distance outside the
        // right/left (top/bottom) plane is (|x| − z·tan)·cos.
        const double x = glm::dot(rel, right);
        if ((std::abs(x) - z * tanH) * cosH > radius) return false;
        const double y = glm::dot(rel, up);
        if ((std::abs(y) - z * tanV) * cosV > radius) return false;
        return true;
    }
};

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// Replace (or create) the mesh for a chunk. The existing mesh's GPU buffers must
// be destroyed first: plain map assignment leaks the old bgfx handles, and the
// static-buffer pool is capped at 4096 (ARCHITECTURE §10) — leaking on every
// decomposition exhausts it into silent invisible-but-solid chunks.
// All-air chunks produce no faces; they are kept OUT of the store entirely so
// the per-frame render loop isn't iterating thousands of empty entries.
void rebuildMesh(MeshStore& ms, const ChunkCoord& cc, const Chunk& chunk) {
    auto it = ms.find(cc);
    if (it != ms.end()) {
        it->second.destroy();
        ms.erase(it);
    }
    ChunkMesh mesh = ChunkMesh::build(chunk);
    if (mesh.empty()) return;  // empty mesh holds no buffers; nothing to keep
    ms.emplace(cc, mesh);
}

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
            if (chunk) rebuildMesh(compMeshes, cc, *chunk);
        }
        // Evicted composite chunks (destroy mesh).
        for (const ChunkCoord& cc : d.evictedCompChunks) {
            auto it = compMeshes.find(cc);
            if (it != compMeshes.end()) { it->second.destroy(); compMeshes.erase(it); }
        }

        // Macro voxels whose block state changed: newly decomposed macros had
        // their block voxel cleared; re-atomized macros had it restored (their
        // children left view range and collapsed back to the coarse block).
        // Either way the owning composite chunk must be remeshed. For top-down
        // eviction the newlyAtomic macro's chunk was itself evicted — getChunk
        // returns null and the remesh is skipped. Many macros can share one
        // chunk; dedupe so each chunk is rebuilt at most once.
        if (compLayer) {
            std::unordered_set<ChunkCoord, ChunkCoordHash> remesh;
            for (const chunkmath::VoxelCoord& macro : d.newlyDecomposed)
                remesh.insert(chunkmath::voxelToChunkLocal(
                    macro, compLayer->chunkSizeVoxels()).chunk);
            for (const chunkmath::VoxelCoord& macro : d.newlyAtomic)
                remesh.insert(chunkmath::voxelToChunkLocal(
                    macro, compLayer->chunkSizeVoxels()).chunk);
            for (const ChunkCoord& cc : remesh) {
                const Chunk* chunk = compLayer->getChunk(cc);
                if (chunk) rebuildMesh(compMeshes, cc, *chunk);
            }
        }

        // New child chunks (build fine mesh).
        for (const ChunkCoord& cc : d.newChildChunks) {
            if (!childLayer) continue;
            const Chunk* chunk = childLayer->getChunk(cc);
            if (chunk) rebuildMesh(childMeshes, cc, *chunk);
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
    // the coarse-supersets-fine invariant (ARCHITECTURE §4, M10). The cascade is
    // intentionally shallow (top 64 m, ratio 4) so a small approach radius can
    // decompose the column under the player without unbounded fine-voxel work:
    //   continental 64 m, chunk 4 →  16 m/chunk  (ratio 4 to regional)
    //   regional    16 m, chunk 4 →   4 m/chunk  (ratio 4 to local)
    //   local        4 m, chunk 4 →   1 m/chunk  (ratio 4 to terrain)
    //   terrain      1 m, chunk 4 →   1 m/chunk  (terminal)
    // Resident budgets keep total mesh count under bgfx's 4096 static-buffer
    // cap (each non-empty chunk mesh = 1 vertex + 1 index buffer); the terrain
    // cap matters most — fine chunks dominate the handle load. The manager
    // collapses the farthest decomposed macros (restoring their coarse blocks)
    // when a cap is exceeded, so the fine bubble shrinks instead of breaking.
    LayerConfig cfg = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: continental
    voxel_size_m: 64.0
    mode: composite
    decompose_to: regional
    chunk_size_voxels: 4
    view_distance_chunks: 3
    resident_chunk_budget: 128

  - name: regional
    voxel_size_m: 16.0
    mode: composite
    decompose_to: local
    chunk_size_voxels: 4
    view_distance_chunks: 4
    resident_chunk_budget: 256

  - name: local
    voxel_size_m: 4.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 4
    view_distance_chunks: 5
    resident_chunk_budget: 1024

  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 4
    view_distance_chunks: 8
    resident_chunk_budget: 2048
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
    // The drill world is a surface slab (top at ~58 m): the entire crust fits in
    // continental chunk row y=0 (a 256 m slab). Banding root streaming to that
    // row avoids loading and iterating ~6× empty sky/underground chunks.
    decompMgr.setVerticalBand(0, 0);

    // ── Renderer ──────────────────────────────────────────────────────────────
    platform::Window window(1280, 720, "VoxelEngine — M10 Drill to the Core");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    // Shallow cascade: continental chunks are 256 m and the resident bubble is a
    // few hundred metres. 4 km far clip comfortably covers it while keeping good
    // depth precision for the 1 m terrain (the default 1000 m would clip context).
    renderer.setFarClip(kFarClipM);
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

    // Remesh a terrain chunk after a voxel edit.
    auto remeshTerrainChunk = [&](const chunkmath::VoxelCoord& vc) {
        const chunkmath::LocalVoxel lv =
            chunkmath::voxelToChunkLocal(vc, terrain->chunkSizeVoxels());
        const Chunk* chunk = terrain->getChunk(lv.chunk);
        MeshStore& ms = meshStores["terrain"];
        auto it = ms.find(lv.chunk);
        if (chunk) {
            if (it != ms.end()) { it->second.destroy(); it->second = ChunkMesh::build(*chunk); }
            else ms.emplace(lv.chunk, ChunkMesh::build(*chunk));
        }
    };

    // Held-to-mine accumulator for terrain edits.
    sim::RemovalAccumulator remover;
    bool prevRight = false;

    // ── Camera / player ───────────────────────────────────────────────────────
    float      pitch = -0.35f, yaw = 0.0f;
    WorldCoord camPos(32.0, 80.0, 32.0);  // just above the 64 m continental slab top
    double     lastMX = 0, lastMY = 0;
    bool       firstMouse = true, cursorCaptured = true;
    bool       prevF = false, prevG = false;
    bool       walkMode = false;
    WorldCoord playerCenter(32.0, 80.0, 32.0);
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
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        // Clamp: cascade decomposition hitches can spike dt to >0.5 s. An
        // unclamped dt yanks walk-mode gravity through the floor (breaking jump)
        // and makes held-to-mine removal fire erratically. Cap at 0.1 s.
        if (dt > 0.1f) dt = 0.1f;

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
        // The manager streams the root composite layer, runs decomposition, and
        // cascade-evicts/collapses chunks that leave view range. The returned
        // diffs tell us which meshes to build or destroy. applyPerFrame bounds
        // how many completed jobs land per frame — and therefore how many meshes
        // this frame builds — so bursts of completions don't hitch.
        auto diffs = decompMgr.tick(camPos, kApproachRadiusM,
                                    /*loadPerFrame=*/32, /*decompPerFrame=*/128,
                                    /*applyPerFrame=*/16);
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

        // ── Terrain editing ───────────────────────────────────────────────────
        {
            const glm::dvec3 lookDir{
                static_cast<double>(std::cos(pitch) * std::sin(yaw)),
                static_cast<double>(std::sin(pitch)),
                static_cast<double>(std::cos(pitch) * std::cos(yaw))};
            voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);

            const bool curLeft  = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS);
            const bool curRight = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

            if (hit.hit && curLeft) {
                const Voxel target = world.getVoxel(
                    chunkmath::voxelCenter(hit.voxel, terrain->voxelSizeM()));
                if (remover.accrue(hit.voxel, target.material.hardness, kToolPower, dt)) {
                    world.setVoxel(chunkmath::voxelCenter(hit.voxel, terrain->voxelSizeM()),
                                   Voxel::empty());
                    remeshTerrainChunk(hit.voxel);
                    remover.reset();
                }
            } else {
                remover.reset();
            }

            if (hit.hit && curRight && !prevRight) {
                Voxel placed;
                placed.material = pm.material("soil");
                // The cell above a surface voxel can fall in a terrain chunk the
                // cascade never created (no ground there = no chunk). Create an
                // empty chunk on demand so placement into open air still lands.
                const ChunkCoord ac =
                    chunkmath::voxelToChunkLocal(hit.adjacent, terrain->chunkSizeVoxels()).chunk;
                if (!terrain->getChunk(ac)) terrain->loadChunk(ac, nullptr, nullptr);
                world.setVoxel(chunkmath::voxelCenter(hit.adjacent, terrain->voxelSizeM()), placed);
                remeshTerrainChunk(hit.adjacent);
            }
            prevRight = curRight;

            if (hit.hit) {
                const float progress =
                    (remover.hasTarget() && remover.target() == hit.voxel)
                        ? remover.progress() : -1.0f;
                renderer.drawVoxelHighlight(
                    chunkmath::voxelCenter(hit.voxel, terrain->voxelSizeM()),
                    static_cast<float>(terrain->voxelSizeM()), 0xff00ffff, progress);
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
                "Decomposed: cont=%d  reg=%d  loc=%d | terrain chunks=%d | in-flight=%d"
                " | LMB=break  RMB=place soil",
                contDecomp, regDecomp, locDecomp, terrChunks, inFlight);
            renderer.setHudText({std::string(hud)});
        }

        // ── Render ────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        Frustum frustum;
        frustum.update(camPos, pitch, yaw,
                       static_cast<double>(fbW) / static_cast<double>(fbH),
                       kVFovDeg, kFarClipM);

        // Render all layers in reverse order (coarsest first, so finer voxels
        // occlude coarser ones via depth test), frustum-culling per chunk —
        // after a long flight the mesh stores hold thousands of chunks, most of
        // them behind the camera or outside the view cone.
        for (const auto& layerName : std::vector<std::string>{
                "continental", "regional", "local", "terrain"}) {
            Layer* lyr = world.layer(layerName);
            if (!lyr) continue;
            const double chunkWorld = lyr->voxelSizeM() * lyr->chunkSizeVoxels();
            const double sphereRadius = chunkWorld * 0.8660254;  // half diagonal, √3/2
            const MeshStore& ms = meshStores[layerName];
            for (const auto& kv : ms) {
                const Chunk* chunk = lyr->getChunk(kv.first);
                if (!chunk || kv.second.empty()) continue;
                const glm::dvec3 center =
                    chunk->origin().value + glm::dvec3(chunkWorld * 0.5);
                if (!frustum.sphereVisible(center, sphereRadius)) continue;
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

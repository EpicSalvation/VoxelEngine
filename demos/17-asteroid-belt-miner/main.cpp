// M16 demo — Asteroid belt miner.
//
// The complementary case to "Beyond blocks": instead of one privileged "down",
// MANY. A rocketsuit miner jets through a dense field of asteroids in zero
// AMBIENT gravity. Each asteroid is a composite voxel that DECOMPOSES on approach
// (M6) into a 1 m minable grid, and exerts its OWN radial gravity well — so the
// player can drop onto, stick to, and walk the surface of a body from ANY side,
// then mine it with the same held-to-break tool from M5/M8.
//
// What it proves about the generalized engine (M16):
//   * Isotropic streaming (L1). Asteroids surround the camera in every direction,
//     so the decomposition cascade streams as a camera-centered BOX volume with
//     no vertical bias — the pre-M16 absolute-Y band would have emptied the
//     moment the player climbed "above" the field. (Configured per-layer in the
//     LayerConfig below; the DecompositionManager streams the root over it.)
//   * Local, many-bodied gravity (L7). There is no world "down". Each frame the
//     suit reads gravityAt() as a RADIAL well aimed at the NEAREST asteroid's
//     center (GravityProvider::radial), recomputed as the player moves between
//     bodies — "down" is wherever the nearest rock is.
//   * Axis-agnostic grounding (L2). VoxelCollision::moveAABB is handed that radial
//     vector as its "down": `grounded` becomes "blocked along the gravity vector",
//     so the suit rests on the +X face of one asteroid as readily as the −Y face
//     of another. Zero-g (between bodies) degenerates to no grounded concept.
//   * Property-driven removal (M8). Mined voxels clear on accrued work vs. their
//     own `hardness` — the worley ore veins (hardness 1.40) take longer to break
//     than the surrounding rock (0.55). No block-type IDs anywhere.
//
// Content comes from the asteroid-field plugin (M16 C1): a radial density field
// presented as a macro→micro→grid cascade. The demo and the plugin share the body
// lattice (plugins/asteroid-field/asteroid_field.h) so the gravity wells point at
// exactly the centers the rock is built around.
//
// Surface-normal camera (M17): in the suit the camera up-axis is aligned to the
// local surface normal (-gDir) via Renderer::setCameraUp, so the horizon reads
// level on whatever face you land on instead of tilted. The mining raycast and
// movement derive look/right from the SAME basis (cameraBasis), so the crosshair
// stays locked to what is mineable. The jet (and zero-g, where there is no "down")
// keeps the plain Y-up basis. The HUD still reads out the live "down" vector and
// grounded state. (The renderer's gravity-relative shade ramp — the G3 fix that
// landed with this — is available too; this flat-colored demo leaves its meshes on
// the default ramp.)
//
// Controls: WASD move, mouse look, Space/Shift up/down (jet) or jump/down (suit),
//           G toggle jet/suit, LMB mine, F cursor, ESC quits.

#include "asteroid_field.h"  // shared body lattice → radial gravity targets

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "core/RecipeValidation.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/CameraBasis.h"
#include "renderer/ChunkMesh.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionManager.h"
#include "world/GravityProvider.h"
#include "world/VoxelCollision.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"
#include "simulation/RemovalAccumulator.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef VOXEL_ASTEROID_FIELD_PLUGIN_PATH
#  define VOXEL_ASTEROID_FIELD_PLUGIN_PATH ""
#endif

namespace {

constexpr char kLogCat[] = "demo17";

// Fallback decompose radius for any composite layer that does NOT set its own
// decompose_distance_m. Both composite layers below specify one (macro = 280 m,
// micro = 90 m — see the LayerConfig), so this value is unused in practice; it is
// kept as the tick() argument for layers that might omit a per-layer radius.
constexpr double kApproachRadiusM = 120.0;
// The cascade steps are now DECOUPLED per layer (decompose_distance_m). The coarse
// macro→micro step fires far out (280 m) so an asteroid's 4 m silhouette takes
// shape ~4 s before arrival, while the expensive micro→grid step fires only up
// close (90 m) so the 1 m mineable grid — the only layer that meaningfully spends
// the renderer's static-buffer handles (empty space meshes to nothing) — stays
// bounded. The grid handle peak (bgfx's static-buffer cap is raised above its 4096
// default via BGFX_CONFIG_MAX_* in CMakeLists.txt) is governed by the SMALL micro
// radius, not the large macro one.
constexpr double kReachM          = 8.0;    // voxel-pick reach in metres
constexpr float  kToolPower       = 4.0f;   // dig speed (work-units/s)
constexpr float  kFlySpeed        = 70.0f;  // jet free-flight speed
constexpr float  kMouseSens       = 0.002f;

// Suit (radial-gravity) kinematics. The well's *direction* comes from L7; the
// demo supplies its own acceleration magnitude (gravityAt's magnitude is only a
// direction carrier — see GravityProvider.h).
constexpr double kWalkSpeed = 7.0;
constexpr double kGravAccel = 22.0;  // m/s² toward the nearest body
constexpr double kJumpSpeed = 8.0;   // m/s away from the body on jump
constexpr double kEyeOffset = 0.6;   // camera sits this far up-axis from center
const glm::dvec3 kPlayerHalf(0.4, 0.4, 0.4);  // isotropic suit — no canonical up

constexpr uint64_t kWorldSeed = 0xA57E401DF1E1Dull;  // matches the field seed

// Valuable-mineral identity. The asteroid-field plugin paints worley ore veins
// into palette slot 31 (its kOreIdx; rock=30, ice=32) — the only valuable, and
// hardness-costliest, material in the field. The HUD's "minerals" tracker counts
// mined-out voxels carrying this palette index, so it never miscounts plain rock
// or ice as ore. Kept in sync with plugins/asteroid-field/plugin.cpp by value.
constexpr uint8_t kOrePaletteIdx = 31;

// The asteroid cascade is dense in 3D, so far clip and FOV match the renderer's
// 60° vertical FOV. 2 km comfortably frames the resident box.
constexpr float  kFarClipM = 2000.0f;

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// Replace (or create) the mesh for a chunk, destroying the old GPU buffers first
// (a plain reassign leaks bgfx handles into the 4096 static-buffer cap — see the
// M6 notes). All-air chunks hold no faces and are kept OUT of the store so the
// render loop never iterates empty entries.
void rebuildMesh(MeshStore& ms, const ChunkCoord& cc, const Chunk& chunk) {
    auto it = ms.find(cc);
    if (it != ms.end()) { it->second.destroy(); ms.erase(it); }
    ChunkMesh mesh = ChunkMesh::build(chunk);
    if (mesh.empty()) return;
    ms.emplace(cc, std::move(mesh));
}

// Apply one tick's DecompositionManager diffs to the per-layer mesh stores
// (identical in shape to demo 10): build meshes for new composite/child chunks,
// destroy evicted ones, remesh composite chunks whose macro block toggled.
void applyDiffs(const std::vector<LayerTickDiff>& diffs, World& world,
                std::unordered_map<std::string, MeshStore>& meshStores) {
    for (const LayerTickDiff& d : diffs) {
        MeshStore& compMeshes  = meshStores[d.compositeLayerName];
        MeshStore& childMeshes = meshStores[d.childLayerName];
        Layer* compLayer  = world.layer(d.compositeLayerName);
        Layer* childLayer = world.layer(d.childLayerName);

        for (const ChunkCoord& cc : d.newCompChunks) {
            if (!compLayer) continue;
            if (const Chunk* chunk = compLayer->getChunk(cc)) rebuildMesh(compMeshes, cc, *chunk);
        }
        for (const ChunkCoord& cc : d.evictedCompChunks) {
            auto it = compMeshes.find(cc);
            if (it != compMeshes.end()) { it->second.destroy(); compMeshes.erase(it); }
        }

        if (compLayer) {
            std::unordered_set<ChunkCoord, ChunkCoordHash> remesh;
            for (const chunkmath::VoxelCoord& macro : d.newlyDecomposed)
                remesh.insert(chunkmath::voxelToChunkLocal(macro, compLayer->chunkSizeVoxels()).chunk);
            for (const chunkmath::VoxelCoord& macro : d.newlyAtomic)
                remesh.insert(chunkmath::voxelToChunkLocal(macro, compLayer->chunkSizeVoxels()).chunk);
            for (const ChunkCoord& cc : remesh)
                if (const Chunk* chunk = compLayer->getChunk(cc)) rebuildMesh(compMeshes, cc, *chunk);
        }

        for (const ChunkCoord& cc : d.newChildChunks) {
            if (!childLayer) continue;
            if (const Chunk* chunk = childLayer->getChunk(cc)) rebuildMesh(childMeshes, cc, *chunk);
        }
        for (const ChunkCoord& cc : d.evictedChildChunks) {
            auto it = childMeshes.find(cc);
            if (it != childMeshes.end()) { it->second.destroy(); childMeshes.erase(it); }
        }
    }
}

}  // namespace

int main() {
    // ── Layer config ──────────────────────────────────────────────────────────
    // A three-level isotropic cascade (parent voxel size == child chunk world
    // size at every step, the clean-alignment rule):
    //   macro 16 m composite → micro   (chunk 4 → 64 m/chunk, ratio 4)
    //   micro  4 m composite → grid    (chunk 4 → 16 m/chunk, ratio 4)
    //   grid   1 m terminal  + interactive (the minable surface; chunk 4 → 4 m)
    // Every layer streams as a BOX (M16 L1) — no vertical bias, because asteroids
    // surround the player in 3D. The root (macro) box is what makes "fly above the
    // field and it stays resident" work where the old Y-band would have emptied.
    //
    // The two cascade steps fire at DECOUPLED distances (decompose_distance_m), so a
    // body refines coarse→fine in legible stages instead of all at once up close:
    //   macro→micro at 280 m — the 4 m silhouette takes shape ~4 s out (70 m/s jet)
    //   micro→grid  at  90 m — the 1 m mineable grid forms only near the player
    //
    // View distances / budgets are sized to the actual cost model, not RAM. Empty
    // space meshes to nothing (rebuildMesh drops all-air chunks), so the coarse
    // layers reach FAR cheaply: macro resolves crude 16 m blobs out to 7·64 = 448 m
    // (> the 280 m macro radius, so macros are resident before they decompose).
    // CRITICAL coupling: micro's view distance (20·16 = 320 m) must EXCEED the macro
    // radius (280 m) — otherwise micro chunks created by macro decomposition would be
    // evicted inside the macro bubble and instantly re-decompose (thrash). micro is
    // non-root, so a large view distance only delays its eviction; it loads nothing.
    //
    // resident_chunk_budget caps CPU-side chunk count (~1.5 KB each — where the spare
    // hundreds of MB go). The renderer's static-buffer cap (raised above bgfx's 4096
    // default via BGFX_CONFIG_MAX_* in CMakeLists.txt) is spent almost entirely on
    // non-empty GRID chunks, now bounded by the SMALL 90 m micro radius. grid's 6144
    // budget is the real shedding mechanism behind the player: inside the 90 m bubble
    // grid is pinned (eviction there would just re-decompose), and the farthest grids
    // beyond it are re-atomized back to 4 m blocks once the resident set exceeds the
    // budget — so memory hovers near the budget rather than growing to micro's range.
    LayerConfig cfg = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: macro
    voxel_size_m: 16.0
    mode: composite
    decompose_to: micro
    chunk_size_voxels: 4
    view_distance_chunks: 7
    resident_chunk_budget: 3500
    decompose_distance_m: 280.0
    streaming_volume:
      shape: box

  - name: micro
    voxel_size_m: 4.0
    mode: composite
    decompose_to: grid
    chunk_size_voxels: 4
    view_distance_chunks: 20
    resident_chunk_budget: 4096
    decompose_distance_m: 90.0
    streaming_volume:
      shape: box

  - name: grid
    voxel_size_m: 1.0
    mode: terminal
    interactive: true
    chunk_size_voxels: 4
    view_distance_chunks: 10
    resident_chunk_budget: 6144
    streaming_volume:
      shape: box
)");
        } catch (const std::exception& e) {
            Log::error(kLogCat, (std::string("Fatal: ") + e.what()).c_str());
            std::exit(1);
        }
    }();

    // ── Plugin loading ──────────────────────────────────────────────────────────
    if (std::string(VOXEL_ASTEROID_FIELD_PLUGIN_PATH).empty()) {
        Log::error(kLogCat, "Fatal: asteroid-field plugin not configured at build time.");
        return 1;
    }

    World world(cfg);
    Engine engine;
    PluginManager pm;
    engine.init(pm, world);

    if (pm.loadPlugin(VOXEL_ASTEROID_FIELD_PLUGIN_PATH) == kInvalidPluginId) {
        Log::error(kLogCat, (std::string("Fatal: could not load asteroid-field plugin from ")
                             + VOXEL_ASTEROID_FIELD_PLUGIN_PATH).c_str());
        return 1;
    }
    try {
        validateRecipes(cfg, pm);
    } catch (const std::exception& e) {
        Log::error(kLogCat, (std::string("Fatal: recipe validation: ") + e.what()).c_str());
        return 1;
    }

    // ── Engine-owned cascade orchestrator ─────────────────────────────────────
    // No setVerticalBand: the field is isotropic, so the root streams in full 3D
    // under its box volume.
    DecompositionManager decompMgr(world, pm, cfg, kWorldSeed);

    Layer* grid = world.layer("grid");
    if (!grid) { Log::error(kLogCat, "Fatal: 'grid' layer not found."); return 1; }

    // ── Renderer ──────────────────────────────────────────────────────────────
    platform::Window window(1280, 720, "VoxelEngine — M16 Asteroid Belt Miner");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setFarClip(kFarClipM);
    renderer.setCrosshair(true);

    std::unordered_map<std::string, MeshStore> meshStores;

    // Remesh a grid chunk after a voxel edit (mining).
    auto remeshGridChunk = [&](const chunkmath::VoxelCoord& vc) {
        const chunkmath::LocalVoxel lv =
            chunkmath::voxelToChunkLocal(vc, grid->chunkSizeVoxels());
        const Chunk* chunk = grid->getChunk(lv.chunk);
        MeshStore& ms = meshStores["grid"];
        auto it = ms.find(lv.chunk);
        if (chunk) {
            if (it != ms.end()) { it->second.destroy(); it->second = ChunkMesh::build(*chunk); }
            else ms.emplace(lv.chunk, ChunkMesh::build(*chunk));
        }
    };

    // Held-to-mine accumulator for grid edits (M8 property-driven removal).
    sim::RemovalAccumulator remover;

    // Running tally of valuable ore voxels mined this session (see kOrePaletteIdx).
    int mineralsMined = 0;

    // ── Camera / player ───────────────────────────────────────────────────────
    // Start in open space looking toward the origin cell, where the field seed
    // reliably places bodies to jet toward.
    float      pitch = -0.1f, yaw = 3.14159f;
    WorldCoord camPos(40.0, 40.0, 150.0);
    double     lastMX = 0, lastMY = 0;
    bool       firstMouse = true, cursorCaptured = true;
    bool       prevF = false, prevG = false;
    bool       suitMode = false;                 // false = jet (free 6-DOF flight)
    WorldCoord playerCenter = camPos;            // AABB center in suit mode
    glm::dvec3 vel(0.0);                          // suit velocity (gravity + jump)
    bool       grounded = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    Log::info(kLogCat, "Controls: WASD move, Space/Shift up-down, G jet/suit, LMB mine, "
                       "F cursor, ESC quit. Jet toward the gray blobs to decompose them; "
                       "press G near a body to drop onto it under its own gravity.");

    auto prevTime = std::chrono::high_resolution_clock::now();

    // ── Main loop ─────────────────────────────────────────────────────────────
    while (!window.shouldClose()) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;  // clamp cascade hitches (and suit gravity)

        window.pollEvents();
        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // ── Mouse look ──────────────────────────────────────────────────────────
        if (cursorCaptured) {
            double mx, my;
            glfwGetCursorPos(glfwWin, &mx, &my);
            if (firstMouse) { lastMX = mx; lastMY = my; firstMouse = false; }
            yaw   += static_cast<float>((mx - lastMX) * kMouseSens);
            pitch -= static_cast<float>((my - lastMY) * kMouseSens);
            pitch  = std::max(-1.5f, std::min(1.5f, pitch));
            lastMX = mx; lastMY = my;
        }

        // ── F: toggle cursor capture ────────────────────────────────────────────
        const bool curF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curF && !prevF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevF = curF;

        // ── G: toggle jet ⇄ suit ────────────────────────────────────────────────
        const bool curG = (glfwGetKey(glfwWin, GLFW_KEY_G) == GLFW_PRESS);
        if (curG && !prevG) {
            suitMode = !suitMode;
            if (suitMode) { playerCenter = camPos; vel = glm::dvec3(0.0); grounded = false; }
        }
        prevG = curG;

        // ── Gravity policy (L7): radial well at the NEAREST asteroid center ──────
        // Rebuilt every frame from the shared body lattice as the player moves
        // between bodies — "down" is wherever the nearest rock is, or nothing in
        // open space (zero-g drift).
        const glm::dvec3& refPos = suitMode ? playerCenter.value : camPos.value;
        glm::dvec3 bodyCenter(0.0);
        double     bodyRadius = 0.0;
        const bool haveBody = asteroidfield::nearestCenter(refPos, bodyCenter, bodyRadius);
        const GravityProvider gravity =
            haveBody ? GravityProvider::radial(WorldCoord(bodyCenter))
                     : GravityProvider::zeroG();
        const glm::dvec3 gRaw = gravity.gravityAt(WorldCoord(refPos));
        const double     gLen = glm::length(gRaw);
        const glm::dvec3 gDir = (gLen > 1e-9) ? gRaw / gLen : glm::dvec3(0.0);  // unit "down"

        // ── Camera basis (M17, surface-normal up) ───────────────────────────────
        // In the suit on a body, align the camera up-axis to the surface normal
        // (-gDir) so the horizon reads level instead of tilted; in the jet (or
        // zero-g, where there is no "down") keep the plain Y-up basis. Look/right
        // are derived from the SAME basis the renderer uses (cameraBasis), so the
        // mining raycast and movement stay locked to screen-center on any face.
        const glm::dvec3 camUp =
            (suitMode && gLen > 1e-9) ? -gDir : glm::dvec3(0.0, 1.0, 0.0);
        const CameraBasis basis = cameraBasis(pitch, yaw, 0.0, camUp);
        const glm::dvec3 look  = basis.forward;
        const glm::dvec3 right = basis.right;

        // ── Movement ────────────────────────────────────────────────────────────
        if (!suitMode) {
            // Jet: pure 6-DOF free flight through zero ambient gravity.
            glm::dvec3 wish(0);
            if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) wish += look;
            if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) wish -= look;
            if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) wish -= glm::dvec3(right);
            if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) wish += glm::dvec3(right);
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) wish.y += 1.0;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) wish.y -= 1.0;
            if (glm::length(wish) > 0.0) wish = glm::normalize(wish);
            camPos = WorldCoord(camPos.value + wish * (kFlySpeed * static_cast<double>(dt)));
        } else {
            // Suit: walk under the body's radial gravity. Move input lives in the
            // tangent plane of the local "up" (−gDir); gravity accelerates the suit
            // toward the body, and moveAABB reports grounded when it presses into a
            // surface ALONG gDir — true on any face of any body (M16 L2).
            const glm::dvec3 up = -gDir;  // away from the body center
            // Build a tangent basis from the camera look, projected off the up
            // axis. Falls back to camera-right if look is parallel to up.
            glm::dvec3 fwdT = look - up * glm::dot(look, up);
            if (glm::length(fwdT) < 1e-6) fwdT = glm::dvec3(right) - up * glm::dot(glm::dvec3(right), up);
            if (glm::length(fwdT) > 1e-9) fwdT = glm::normalize(fwdT);
            glm::dvec3 rightT = glm::cross(fwdT, up);
            if (glm::length(rightT) > 1e-9) rightT = glm::normalize(rightT);

            glm::dvec3 wish(0);
            if (glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS) wish += fwdT;
            if (glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS) wish -= fwdT;
            if (glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS) wish -= rightT;
            if (glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS) wish += rightT;
            if (glm::length(wish) > 0.0) wish = glm::normalize(wish);

            // Gravity acceleration along gDir (no-op in open space where gDir = 0);
            // jump adds a one-shot velocity impulse along the local up.
            vel += gDir * (kGravAccel * static_cast<double>(dt));
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE) == GLFW_PRESS && grounded && gLen > 1e-9)
                vel += up * kJumpSpeed;

            // Lateral walk plus an optional direct "press toward the body" thrust
            // (Shift) — both are direct deltas, not integrated into vel.
            glm::dvec3 thrust = wish * kWalkSpeed;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
                thrust += gDir * kWalkSpeed;
            glm::dvec3 delta = thrust * static_cast<double>(dt)
                             + vel    * static_cast<double>(dt);
            auto mr = voxelcollide::moveAABB(world, {playerCenter, kPlayerHalf}, delta, gDir);
            playerCenter = mr.position;
            grounded = mr.grounded;
            // Resting on a surface: kill the velocity component pushing into it.
            if (grounded) {
                const double into = glm::dot(vel, gDir);  // >0 means moving "down"
                if (into > 0.0) vel -= gDir * into;
            }
            camPos = WorldCoord(playerCenter.value + up * kEyeOffset);
        }

        // ── DecompositionManager tick ─────────────────────────────────────────
        auto diffs = decompMgr.tick(camPos, kApproachRadiusM,
                                    /*loadPerFrame=*/64, /*decompPerFrame=*/256,
                                    /*applyPerFrame=*/32);
        applyDiffs(diffs, world, meshStores);

        // Drop grid meshes whose chunk left the resident set (cascade-evicted).
        {
            MeshStore& gridMeshes = meshStores["grid"];
            std::vector<ChunkCoord> stale;
            for (const auto& kv : gridMeshes)
                if (!grid->getChunk(kv.first)) stale.push_back(kv.first);
            for (const ChunkCoord& cc : stale) {
                gridMeshes[cc].destroy();
                gridMeshes.erase(cc);
            }
        }

        // ── Mining (held-to-break, M8) ──────────────────────────────────────────
        {
            voxelcast::RayHit hit = voxelcast::raycast(world, camPos, look, kReachM);
            const bool curLeft = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);

            if (hit.hit && curLeft) {
                const WorldCoord tgt = chunkmath::voxelCenter(hit.voxel, grid->voxelSizeM());
                const Voxel target = world.getVoxel(tgt);
                if (remover.accrue(hit.voxel, target.material.hardness, kToolPower, dt)) {
                    if (target.material.palette_index == kOrePaletteIdx) ++mineralsMined;
                    world.setVoxel(tgt, Voxel::empty());
                    remeshGridChunk(hit.voxel);
                    remover.reset();
                }
            } else {
                remover.reset();
            }

            if (hit.hit) {
                const float progress =
                    (remover.hasTarget() && remover.target() == hit.voxel)
                        ? remover.progress() : -1.0f;
                renderer.drawVoxelHighlight(
                    chunkmath::voxelCenter(hit.voxel, grid->voxelSizeM()),
                    static_cast<float>(grid->voxelSizeM()), 0xff00ffff, progress);
            }
        }

        // ── HUD ─────────────────────────────────────────────────────────────────
        {
            char downStr[48];
            if (gLen > 1e-9)
                std::snprintf(downStr, sizeof(downStr), "down=(%.2f,%.2f,%.2f)",
                              gDir.x, gDir.y, gDir.z);
            else
                std::snprintf(downStr, sizeof(downStr), "down=none (zero-g)");

            const int macroD = static_cast<int>(decompMgr.decomposedCount("macro"));
            const int microD = static_cast<int>(decompMgr.decomposedCount("micro"));
            const int gridChunks = static_cast<int>(grid->chunks().size());
            const int inFlight = static_cast<int>(decompMgr.inFlight());

            char hud[288];
            std::snprintf(hud, sizeof(hud),
                "%s | %s | %s | decomp macro=%d micro=%d | grid=%d | in-flight=%d | minerals=%d | LMB mine",
                suitMode ? "SUIT" : "JET",
                downStr,
                suitMode ? (grounded ? "grounded" : "airborne") : "free-flight",
                macroD, microD, gridChunks, inFlight, mineralsMined);
            renderer.setHudText({std::string(hud)});
        }

        // ── Render ────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        renderer.setCameraUp(glm::vec3(camUp));  // surface-normal up in the suit; +Y in jet/zero-g

        // Coarsest first so finer voxels occlude coarser ones via the depth test.
        for (const auto& layerName : std::vector<std::string>{"macro", "micro", "grid"}) {
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

    // ── Cleanup ───────────────────────────────────────────────────────────────
    for (auto& [name, ms] : meshStores)
        for (auto& [cc, mesh] : ms)
            mesh.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

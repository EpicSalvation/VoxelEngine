// M5 demo — build, break, and persist.
//
// This demo grows across the M5 task groups. Implemented so far:
//   - place/remove: a plugin-driven streaming world (the M4 base-terrain
//     heightmap) the player can edit. A double-precision DDA raycast finds the
//     targeted voxel (wireframe highlight + crosshair); left mouse breaks, right
//     mouse places the selected material. Every edit fires on_voxel_modified and
//     re-meshes the affected chunk.
//   - persistence: edited (dirty) chunks are written to a save directory — on
//     eviction (save-then-evict) and on quit. On relaunch the saved chunks load
//     from disk in place of the generator, so edits survive across runs while
//     untouched terrain regenerates deterministically.
//   - collision: press G to drop into a walking player — a kinematic AABB with
//     gravity, jumping, and swept terminal-voxel collision (slides along walls,
//     never tunnels). G again returns to the free-fly camera.
//
// Controls: WASD move, mouse look, F toggles cursor, G toggles walk/fly,
// Space/Shift up/down (fly) or jump (walk), left/right mouse break/place,
// 1-9 select build material, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "io/ChunkPersistence.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/VoxelCollision.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <vector>

#ifndef VOXEL_BASE_PLUGIN_PATH
#  define VOXEL_BASE_PLUGIN_PATH ""
#endif

namespace {
constexpr int    kLoadsPerFrame = 2;      // budget generated/meshed chunks per frame
constexpr float  kFlySpeed      = 24.0f;  // free-fly camera speed
constexpr float  kMouseSens     = 0.002f;
constexpr double kReachM        = 8.0;    // how far the player can target a voxel

// Walking player (walk mode).
constexpr double kWalkSpeed = 6.0;        // m/s horizontal
constexpr double kGravity   = 25.0;       // m/s^2
constexpr double kJumpSpeed = 8.0;        // m/s initial upward
constexpr double kEyeOffset = 0.7;        // eye height above the AABB center
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);  // half-extents: 0.6 wide, 1.8 tall
}  // namespace

int main() {
    LayerConfig layerConfig = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 32
    view_distance_chunks: 5
)");
        } catch (const std::exception& e) {
            std::cerr << "[main] Fatal: layer config error: " << e.what() << "\n";
            std::exit(1);
        }
    }();
    const LayerDef& terrain = layerConfig.layers().front();

    // The world's materials and terrain come entirely from the base-terrain plugin.
    PluginManager pluginManager;
    if (std::string(VOXEL_BASE_PLUGIN_PATH).empty()) {
        std::cerr << "[main] Fatal: base plugin path not configured at build time.\n";
        return 1;
    }
    if (pluginManager.loadPlugin(VOXEL_BASE_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load base-terrain plugin from "
                  << VOXEL_BASE_PLUGIN_PATH << "\n";
        return 1;
    }

    LayerGeneratorFn generator = nullptr;
    void*            generatorUserData = nullptr;
    for (const auto& g : pluginManager.layerGenerators()) {
        if (g.layer_name == "terrain") {
            generator = g.fn;
            generatorUserData = g.user_data;
        }
    }
    if (!generator) {
        std::cerr << "[main] Fatal: no 'terrain' layer generator registered.\n";
        return 1;
    }

    // Build-material palette: every registered material is selectable (keys 1-9).
    const auto& materials = pluginManager.materials();
    if (materials.empty()) {
        std::cerr << "[main] Fatal: no materials registered by the plugin.\n";
        return 1;
    }
    size_t selectedMaterial = 0;
    std::cout << "[main] Build materials (press the number to select):\n";
    for (size_t i = 0; i < materials.size() && i < 9; ++i)
        std::cout << "       " << (i + 1) << " - " << materials[i].material_id << "\n";

    Engine engine;
    engine.start();

    platform::Window window(1024, 768, "VoxelEngine — M5 Build, Break, Persist");

    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    World world(terrain);
    LODManager lod(layerConfig);
    lod.setVerticalBand(0, 0);

    // Persistence: dirty chunks are saved here and reloaded on relaunch. The
    // identity ties a save to this layer's voxel/chunk size.
    persistence::WorldSave save("voxelsave",
        persistence::WorldIdentity{terrain.voxel_size_m, terrain.chunk_size_voxels});
    std::cout << "[main] Save directory: " << save.directory() << "\n";

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    // (Re)build the GPU mesh for the chunk that owns a just-edited voxel. The
    // mesher always emits opaque border faces (no cross-chunk culling, see
    // ARCHITECTURE §9), so an opaque edit on a chunk seam needs only the owning
    // chunk rebuilt — the neighbor's coincident face is already present.
    auto remeshChunkOf = [&](const chunkmath::VoxelCoord& vc) {
        ChunkCoord cc = chunkmath::voxelToChunkLocal(vc, world.chunkSizeVoxels()).chunk;
        const Chunk* chunk = world.getChunk(cc);
        if (!chunk) return;
        auto it = meshes.find(cc);
        if (it != meshes.end()) {
            it->second.destroy();
            it->second = ChunkMesh::build(*chunk);
        } else {
            meshes.emplace(cc, ChunkMesh::build(*chunk));
        }
    };

    // Apply an edit at a voxel cell: write it, fire the on_voxel_modified hooks
    // with the old/new voxel, and re-mesh the affected chunk. No-op if the cell's
    // chunk is not resident (setVoxel returns false).
    auto editVoxel = [&](const chunkmath::VoxelCoord& vc, const Voxel& newVox) {
        const WorldCoord center = chunkmath::voxelCenter(vc, world.voxelSizeM());
        const Voxel oldVox = world.getVoxel(center);
        if (!world.setVoxel(center, newVox)) return;
        for (const auto& h : pluginManager.voxelModifiedHooks())
            if (h.fn) h.fn(center, &oldVox, &newVox, h.user_data);
        remeshChunkOf(vc);
    };

    // Camera: start above the terrain, free-look enabled (walking + collision is a
    // later M5 group).
    float      pitch = -0.5f, yaw = 0.0f;
    WorldCoord camPos(0.0, 45.0, 0.0);
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true;
    bool       cursorCaptured = true;
    bool       prevKeyF = false, prevKeyG = false;
    bool       prevLeft = false, prevRight = false;

    // Walking player state (active when walkMode is true). The eye (camPos) sits
    // kEyeOffset above the AABB center.
    bool       walkMode = false;
    WorldCoord playerCenter(0.0, 0.0, 0.0);
    double     vy = 0.0;       // vertical velocity
    bool       grounded = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    std::cout << "[main] Build/break the world. WASD + mouse to fly, Space/Shift up/down,\n"
                 "[main] left mouse = break, right mouse = place, 1-9 = material, F = cursor, ESC quits.\n";

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

        // G — toggle between the free-fly camera and the walking player.
        bool curKeyG = (glfwGetKey(glfwWin, GLFW_KEY_G) == GLFW_PRESS);
        if (curKeyG && !prevKeyG) {
            walkMode = !walkMode;
            if (walkMode) {
                playerCenter = WorldCoord(camPos.value - glm::dvec3(0.0, kEyeOffset, 0.0));
                vy = 0.0;
                grounded = false;
            }
            std::cout << "[main] Mode: " << (walkMode ? "WALK (gravity + collision)"
                                                      : "FLY") << "\n";
        }
        prevKeyG = curKeyG;

        // Material selection (1-9).
        for (int i = 0; i < 9 && i < static_cast<int>(materials.size()); ++i) {
            if (glfwGetKey(glfwWin, GLFW_KEY_1 + i) == GLFW_PRESS &&
                selectedMaterial != static_cast<size_t>(i)) {
                selectedMaterial = static_cast<size_t>(i);
                std::cout << "[main] Selected material: "
                          << materials[selectedMaterial].material_id << "\n";
            }
        }

        // Mouse look.
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
            // Free-fly: camera-relative horizontal, world-up vertical.
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
            // Walking: horizontal movement on the yaw plane (pitch ignored), with
            // gravity, jumping, and swept AABB collision against terminal voxels.
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
            if (mr.grounded || (mr.hitY && vy > 0.0)) vy = 0.0;  // stop on floor/ceiling
            camPos = WorldCoord(playerCenter.value + glm::dvec3(0.0, kEyeOffset, 0.0));
        }

        // ── Stream chunks around the camera ──────────────────────────────
        ChunkCoord center =
            chunkmath::worldToChunk(camPos, world.voxelSizeM(), world.chunkSizeVoxels());

        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain")) {
            if (meshes.count(c)) continue;
            // Prefer a saved (player-edited) chunk over generating it. A loaded
            // chunk is authoritative — it does not re-run the generator or any
            // feature generators.
            if (!world.getChunk(c) && save.hasChunk(c)) {
                if (auto disk = save.tryLoadChunk(c))
                    world.insertChunk(std::move(disk));
            }
            Chunk* chunk = world.loadChunk(c, generator, generatorUserData);
            if (!chunk) continue;
            meshes.emplace(c, ChunkMesh::build(*chunk));
            if (++loaded >= kLoadsPerFrame) break;
        }

        // Evict chunks outside the view budget. A dirty chunk is saved before its
        // in-memory copy is dropped (save-then-evict), so edits are never lost;
        // clean chunks are dropped directly and regenerate on cache miss.
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, "terrain"))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& c : toEvict) {
            if (world.isChunkDirty(c)) {
                if (const Chunk* ch = world.getChunk(c)) save.saveChunk(*ch);
                world.clearChunkDirty(c);
            }
            meshes[c].destroy();
            meshes.erase(c);
            world.unloadChunk(c);
        }

        // ── Targeting and edits ──────────────────────────────────────────
        glm::dvec3 lookDir{static_cast<double>(cp * sy), static_cast<double>(sp),
                           static_cast<double>(cp * cy)};
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);
        if (hit.hit)
            renderer.drawVoxelHighlight(
                chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()),
                static_cast<float>(world.voxelSizeM()));

        bool curLeft  = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS);
        bool curRight = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        if (hit.hit && curLeft && !prevLeft) {
            editVoxel(hit.voxel, Voxel::empty());  // break
        }
        if (hit.hit && curRight && !prevRight) {
            // Don't place a block into the space the player occupies: the full
            // AABB in walk mode, or just the eye cell in fly mode.
            const double vs = world.voxelSizeM();
            const chunkmath::VoxelCoord t = hit.adjacent;
            bool blockedByPlayer;
            if (walkMode) {
                glm::dvec3 cmin{static_cast<double>(t.x) * vs,
                                static_cast<double>(t.y) * vs,
                                static_cast<double>(t.z) * vs};
                glm::dvec3 cmax = cmin + glm::dvec3(vs, vs, vs);
                glm::dvec3 pmin = playerCenter.value - kPlayerHalf;
                glm::dvec3 pmax = playerCenter.value + kPlayerHalf;
                blockedByPlayer = (pmin.x < cmax.x && pmax.x > cmin.x &&
                                   pmin.y < cmax.y && pmax.y > cmin.y &&
                                   pmin.z < cmax.z && pmax.z > cmin.z);
            } else {
                blockedByPlayer = (t == chunkmath::worldToVoxel(camPos, vs));
            }
            if (!blockedByPlayer) {
                Voxel placed;
                placed.material = materials[selectedMaterial].props;
                editVoxel(t, placed);
            }
        }
        prevLeft  = curLeft;
        prevRight = curRight;

        // Resize.
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        // Render.
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        for (const auto& kv : meshes) {
            const Chunk* chunk = world.getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin());
        }
        renderer.render();
    }

    // Persist any edits still resident so they survive the next launch.
    int savedOnQuit = save.saveDirtyChunks(world);
    std::cout << "[main] Saved " << savedOnQuit << " edited chunk(s) to "
              << save.directory() << "\n";

    for (auto& kv : meshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

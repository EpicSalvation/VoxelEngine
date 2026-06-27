// M8 demo — material matters.
//
// A flat strata world built by the `material-showcase` plugin: soft topsoil over
// progressively harder rock (grass → dirt → stone → iron → diamond) sitting on an
// indestructible bedrock floor. Hold left mouse to mine the targeted voxel; the
// effort to clear it is a pure function of the material's `hardness` via the M8
// RemovalModel — softer strata clear quickly, harder ones take visibly longer,
// and bedrock (`hardness < 0`) never clears at all. There is no block-type branch
// anywhere on the removal path: the cost is read off the targeted voxel's own
// MaterialProperties (ARCHITECTURE.md §5).
//
// The targeted voxel's hardness / density / structural_strength are shown in the
// HUD, and the wireframe highlight ramps toward red as removal work accrues, so
// the property-driven difference between materials is directly visible. Right
// mouse places the selected material (1-6) — placing diamond and trying to mine
// it back demonstrates a hard-but-removable material; placing bedrock makes an
// indestructible block.
//
// Controls: WASD move, mouse look, F toggles cursor, G toggles walk/fly,
// Space/Shift up/down (fly) or jump (walk), hold left mouse = mine, right mouse =
// place, 1-6 select material, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "simulation/RemovalAccumulator.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/VoxelCollision.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VOXEL_SHOWCASE_PLUGIN_PATH
#  define VOXEL_SHOWCASE_PLUGIN_PATH ""
#endif

namespace {
constexpr char   kLogCat[] = "demo08";
constexpr int    kLoadsPerFrame = 2;      // budget generated/meshed chunks per frame
constexpr float  kFlySpeed      = 18.0f;  // free-fly camera speed
constexpr float  kMouseSens     = 0.002f;
constexpr double kReachM        = 8.0;    // how far the player can target a voxel
constexpr float  kToolPower     = 0.5f;   // removal work-units/sec (RemovalModel)

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
            Log::error(kLogCat, (std::string("Fatal: layer config error: ") + e.what()).c_str());
            std::exit(1);
        }
    }();
    const LayerDef& terrain = layerConfig.layers().front();

    // Materials and the strata world come from the material-showcase plugin.
    PluginManager pluginManager;
    if (std::string(VOXEL_SHOWCASE_PLUGIN_PATH).empty()) {
        Log::error(kLogCat, "Fatal: material-showcase plugin path not configured at build time.");
        return 1;
    }
    if (pluginManager.loadPlugin(VOXEL_SHOWCASE_PLUGIN_PATH) == kInvalidPluginId) {
        Log::error(kLogCat, (std::string("Fatal: could not load material-showcase plugin from ")
                             + VOXEL_SHOWCASE_PLUGIN_PATH).c_str());
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
        Log::error(kLogCat, "Fatal: no 'terrain' layer generator registered.");
        return 1;
    }

    // Build-material palette: the registered materials are selectable (keys 1-N).
    // Ids are captured once; props are resolved at placement time via
    // PluginManager::material(), never by indexing the live registry positionally.
    std::vector<std::string> buildMaterials;
    for (size_t i = 0; i < pluginManager.materials().size() && i < 9; ++i)
        buildMaterials.push_back(pluginManager.materials()[i].material_id);
    if (buildMaterials.empty()) {
        Log::error(kLogCat, "Fatal: no materials registered by the plugin.");
        return 1;
    }
    size_t selectedMaterial = 0;
    Log::info(kLogCat, "Build materials (press the number to select):");
    for (size_t i = 0; i < buildMaterials.size(); ++i) {
        const MaterialProperties m = pluginManager.material(buildMaterials[i]);
        const std::string line = "  " + std::to_string(i + 1) + " - " + buildMaterials[i]
            + "  (hardness " + (m.hardness < 0.0f ? std::string("INDESTRUCTIBLE")
                                                  : std::to_string(m.hardness)) + ")";
        Log::info(kLogCat, line.c_str());
    }

    Engine engine;
    engine.start();

    platform::Window window(1024, 768, "VoxelEngine — M8 Material Matters");

    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    World world(terrain);
    LODManager lod(layerConfig);
    lod.setVerticalBand(0, 0);

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    // (Re)build the GPU mesh for the chunk that owns a just-edited voxel. Opaque
    // border faces mean only the owning chunk needs rebuilding (ARCHITECTURE §9).
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

    // Apply an edit at a voxel cell: write it, fire on_voxel_modified, re-mesh.
    auto editVoxel = [&](const chunkmath::VoxelCoord& vc, const Voxel& newVox) {
        const WorldCoord center = chunkmath::voxelCenter(vc, world.voxelSizeM());
        const Voxel oldVox = world.getVoxel(center);
        if (!world.setVoxel(center, newVox)) return;
        for (const auto& h : pluginManager.voxelModifiedHooks())
            if (h.fn) h.fn(center, &oldVox, &newVox, kLocalPlayer, h.user_data);
        remeshChunkOf(vc);
    };

    // Camera: start above the strata surface (topsoil sits at y=23), looking down.
    float      pitch = -0.7f, yaw = 0.0f;
    WorldCoord camPos(0.0, 30.0, 0.0);
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true;
    bool       cursorCaptured = true;
    bool       prevKeyF = false, prevKeyG = false;
    bool       prevRight = false;

    // Held-to-mine state: per-target removal progress (M8). Transient — reset when
    // the target changes or the action is released.
    sim::RemovalAccumulator remover;

    // Walking player state (active when walkMode is true).
    bool       walkMode = false;
    WorldCoord playerCenter(0.0, 0.0, 0.0);
    double     vy = 0.0;
    bool       grounded = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    Log::info(kLogCat, "Dig down through the strata. Hold left mouse to mine — softer "
                       "materials clear fast, harder ones take longer, and the bedrock floor "
                       "(hardness < 0) never clears. Right mouse places the selected material, "
                       "1-6 selects, F = cursor, G = walk, ESC quits.");

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
            Log::info(kLogCat, walkMode ? "Mode: WALK (gravity + collision)" : "Mode: FLY");
        }
        prevKeyG = curKeyG;

        // Material selection (1-N).
        for (int i = 0; i < static_cast<int>(buildMaterials.size()); ++i) {
            if (glfwGetKey(glfwWin, GLFW_KEY_1 + i) == GLFW_PRESS &&
                selectedMaterial != static_cast<size_t>(i)) {
                selectedMaterial = static_cast<size_t>(i);
                Log::info(kLogCat, (std::string("Selected material: ")
                                    + buildMaterials[selectedMaterial]).c_str());
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

        // ── Stream chunks around the camera ──────────────────────────────
        ChunkCoord center =
            chunkmath::worldToChunk(camPos, world.voxelSizeM(), world.chunkSizeVoxels());

        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain")) {
            if (meshes.count(c)) continue;
            Chunk* chunk = world.loadChunk(c, generator, generatorUserData);
            if (!chunk) continue;
            meshes.emplace(c, ChunkMesh::build(*chunk));
            if (++loaded >= kLoadsPerFrame) break;
        }

        // Evict chunks outside the view budget, but keep edited (dirty) chunks
        // resident so session edits are never silently lost (this demo does not
        // persist to disk — the strata regenerate deterministically on reload).
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, "terrain") && !world.isChunkDirty(kv.first))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& c : toEvict) {
            meshes[c].destroy();
            meshes.erase(c);
            world.unloadChunk(c);
        }

        // ── Targeting and edits ──────────────────────────────────────────
        glm::dvec3 lookDir{static_cast<double>(cp * sy), static_cast<double>(sp),
                           static_cast<double>(cp * cy)};
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);

        bool curLeft  = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS);
        bool curRight = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        // The targeted voxel's material — drives both the removal cost and the HUD.
        // Read off the voxel itself, never a block-type id (ARCHITECTURE §5).
        const Voxel target =
            hit.hit ? world.getVoxel(chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()))
                    : Voxel::empty();

        // Break: holding left mouse accrues removal work scaled by tool power; the
        // voxel clears only when accrued work meets its hardness-derived threshold.
        // An indestructible target (hardness < 0) never accrues and never clears.
        if (hit.hit && curLeft) {
            if (remover.accrue(hit.voxel, target.material.hardness, kToolPower, dt)) {
                editVoxel(hit.voxel, Voxel::empty());  // break
                remover.reset();
            }
        } else {
            remover.reset();
        }
        if (hit.hit && curRight && !prevRight) {
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
                placed.material = pluginManager.material(buildMaterials[selectedMaterial]);
                editVoxel(t, placed);
            }
        }
        prevRight = curRight;

        // Removal-progress feedback: outline the targeted voxel (ramping toward red
        // as removal work accrues) and read out the target's hardness / density /
        // structural_strength in the HUD. An indestructible target is labelled so —
        // the highlight stays plain because no progress ever accrues. All driven by
        // the voxel's own material properties; no block-type branch (ARCHITECTURE §5).
        if (hit.hit) {
            const bool indestructible = target.material.hardness < 0.0f;
            const float progress =
                (remover.hasTarget() && remover.target() == hit.voxel)
                    ? remover.progress() : -1.0f;
            renderer.drawVoxelHighlight(
                chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()),
                static_cast<float>(world.voxelSizeM()), 0xff00ffff, progress);
            char line0[96];
            if (indestructible) {
                std::snprintf(line0, sizeof(line0), "hardness INDESTRUCTIBLE  (cannot mine)");
            } else {
                std::snprintf(line0, sizeof(line0), "hardness %.1f  (mine: hold left mouse)",
                              static_cast<double>(target.material.hardness));
            }
            char line1[96];
            std::snprintf(line1, sizeof(line1), "density %.0f   structural %.1f",
                          static_cast<double>(target.material.density),
                          static_cast<double>(target.material.structural_strength));
            renderer.setHudText({std::string(line0), std::string(line1)});
        } else {
            renderer.setHudText({});
        }

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

    for (auto& kv : meshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

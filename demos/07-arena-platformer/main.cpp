// M7b demo — arena platformer (Groups 1-3: world generation, decomposition, and platformer mechanics).
//
// A five-layer walled arena exercises the full M1–M6 feature set at once:
//
//   "foundation"  500 m immutable — solid stone floor covering the 500×500 m arena.
//   "ramparts"     20 m immutable — 40 m-thick perimeter walls, 100 m tall.
//   "terraces"     10 m composite — platform blocks that decompose into the detail
//                  layer as the player approaches (single-step, M6 pattern).
//   "props"         2 m immutable — decorative columns at eight arena positions.
//   "detail"        1 m terminal  — fine walkable surface revealed by decomposition.
//
// Layer ratio chain: 25:1, 2:1, 5:1, 2:1 — all validated at startup.
//
// Five-layer cross-layer collision: World::anySolidAt samples all five layers so
// the player stands on the 500 m floor, the 20 m walls, the 2 m columns, and the
// 1 m/10 m platform surfaces simultaneously (G to toggle walk/fly).
//
// A feature generator stamps gold key-marker voxels above each non-goal platform;
// these are applied to detail chunks after base generation (M4 hook).
//
// Platformer mechanics (Group 3):
//   - Walk mode: kinematic body with gravity, jumping, and swept AABB collision
//     across all five layers (the M5 kinematic body, wired to World::anySolidAt).
//   - Free-fly camera (F): floating-origin submission keeps sub-meter precision
//     across the full 500 m arena (M2).
//   - Build/break: left mouse breaks a 1 m detail voxel, right mouse places the
//     selected material (1–9). Edited chunks are re-meshed immediately and fire
//     on_voxel_modified. The DDA raycast targets only resident detail layer voxels.
//   - Persistence: dirty detail chunks are saved to "arena-save/" on eviction and
//     on quit. On relaunch saved chunks are preferred over re-generating from the
//     detail generator, so player-built bridges survive across sessions.
//
// Controls: WASD move, mouse look, Space/Shift fly up/down (or jump in walk mode),
// G = walk (gravity + cross-layer AABB collision), F = cursor, left mouse = break,
// right mouse = place, 1–9 = material, ESC quits.
//
// Run from the build directory:
//   ./build/07-arena-platformer
// Fly toward the stone platforms to see them decompose into grass-topped 1 m detail.

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
#include "world/DecompositionWorker.h"
#include "world/MacroVoxel.h"
#include "world/VoxelCollision.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef VOXEL_ARENA_PLUGIN_PATH
#  define VOXEL_ARENA_PLUGIN_PATH ""
#endif

namespace {

// ── Streaming / decomposition budgets ───────────────────────────────────────
constexpr int    kStreamPerFrame     =  2;     // coarse/immutable chunks meshed per frame
constexpr int    kDecomposePerFrame  = 32;     // terrace macro voxels enqueued per frame
constexpr double kDecomposeRadiusM   = 80.0;   // decompose terraces within this radius
// Decomposed detail is released when it drifts past this radius so the resident
// detail-mesh count stays below the bgfx static-buffer ceiling (4096 each).
constexpr double kDetailKeepRadiusM  = 100.0;

// ── Camera / player constants ────────────────────────────────────────────────
constexpr float  kFlySpeed   = 50.0f;
constexpr float  kMouseSens  = 0.002f;
constexpr double kWalkSpeed  =  8.0;
constexpr double kGravity    = 25.0;
constexpr double kJumpSpeed  =  9.0;
constexpr double kEyeOffset  =  0.7;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

// ── Build / edit constants ────────────────────────────────────────────────────
constexpr double kReachM = 8.0;  // how far the player can target a detail voxel

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// Returns the detail-layer child chunks that cover one terrace macro voxel.
// With terraces at 10 m and detail chunks at 10 m (chunk_size=10, vs=1),
// span = round(10 / 10) = 1 → one detail chunk per terrace voxel.
std::vector<ChunkCoord> childChunksForMacro(const chunkmath::VoxelCoord& macro,
                                            const Layer& parent, const Layer& child) {
    const double parentVoxel   = parent.voxelSizeM();
    const double childChunkSz  = child.voxelSizeM() * child.chunkSizeVoxels();
    const int    span = std::max(1, static_cast<int>(std::llround(parentVoxel / childChunkSz)));
    const WorldCoord origin = chunkmath::voxelOrigin(macro, parentVoxel);
    const ChunkCoord base   =
        chunkmath::worldToChunk(origin, child.voxelSizeM(), child.chunkSizeVoxels());

    std::vector<ChunkCoord> out;
    out.reserve(static_cast<size_t>(span) * span * span);
    for (int dz = 0; dz < span; ++dz)
        for (int dy = 0; dy < span; ++dy)
            for (int dx = 0; dx < span; ++dx)
                out.push_back(ChunkCoord{base.x + dx, base.y + dy, base.z + dz});
    return out;
}

}  // namespace

int main() {
    // ── Five-layer arena config ───────────────────────────────────────────────
    // Ratio chain: foundation(500)/ramparts(20)=25, ramparts(20)/terraces(10)=2,
    //              terraces(10)/props(2)=5, props(2)/detail(1)=2. All validated.
    LayerConfig layerConfig = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: foundation
    voxel_size_m: 500.0
    mode: immutable
    chunk_size_voxels: 4
    view_distance_chunks: 1
  - name: ramparts
    voxel_size_m: 20.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 4
  - name: terraces
    voxel_size_m: 10.0
    mode: composite
    decompose_to: detail
    chunk_size_voxels: 8
    view_distance_chunks: 4
  - name: props
    voxel_size_m: 2.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 8
  - name: detail
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 10
    view_distance_chunks: 12
)");
        } catch (const std::exception& e) {
            std::cerr << "[main] Fatal: layer config error: " << e.what() << "\n";
            std::exit(1);
        }
    }();

    // ── Arena plugin ─────────────────────────────────────────────────────────
    PluginManager pluginManager;
    if (std::string(VOXEL_ARENA_PLUGIN_PATH).empty()) {
        std::cerr << "[main] Fatal: arena plugin path not configured at build time.\n";
        return 1;
    }
    if (pluginManager.loadPlugin(VOXEL_ARENA_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load arena plugin from "
                  << VOXEL_ARENA_PLUGIN_PATH << "\n";
        return 1;
    }

    // Look up each layer generator by name.
    auto findGen = [&](const std::string& name) -> RegisteredLayerGenerator {
        for (const auto& g : pluginManager.layerGenerators())
            if (g.layer_name == name) return g;
        return RegisteredLayerGenerator{name, nullptr, nullptr, kInvalidPluginId};
    };
    const auto foundationGen = findGen("foundation");
    const auto rampartsGen   = findGen("ramparts");
    const auto terracesGen   = findGen("terraces");
    const auto propsGen      = findGen("props");
    const auto detailGen     = findGen("detail");

    if (!foundationGen.fn || !rampartsGen.fn || !terracesGen.fn ||
        !propsGen.fn      || !detailGen.fn) {
        std::cerr << "[main] Fatal: arena plugin did not register all five "
                     "layer generators.\n";
        return 1;
    }

    // Build-material palette: every registered material is selectable (keys 1-9).
    const auto& materials = pluginManager.materials();
    if (materials.empty()) {
        std::cerr << "[main] Fatal: no materials registered by the arena plugin.\n";
        return 1;
    }
    size_t selectedMaterial = 0;
    std::cout << "[main] Build materials (press the number to select):\n";
    for (size_t i = 0; i < materials.size() && i < 9; ++i)
        std::cout << "       " << (i + 1) << " - " << materials[i].material_id << "\n";

    // ── Engine + window + renderer ────────────────────────────────────────────
    Engine engine;
    engine.start();

    platform::Window window(1280, 720, "VoxelEngine — M7b Arena Platformer");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    // ── World + layer handles ─────────────────────────────────────────────────
    World world(layerConfig);
    Layer* foundation = world.layer("foundation");
    Layer* ramparts   = world.layer("ramparts");
    Layer* terraces   = world.layer("terraces");
    Layer* props      = world.layer("props");
    Layer* detail     = world.layer("detail");
    if (!foundation || !ramparts || !terraces || !props || !detail) {
        std::cerr << "[main] Fatal: expected all five arena layers.\n";
        return 1;
    }

    // ── Persistence: dirty detail chunks survive across launches ─────────────
    // The "detail" layer is the terminal layer and the only one the player edits.
    // Dirty chunks are saved on eviction (save-then-evict) and on quit.
    const LayerDef* detailDef = layerConfig.findLayer("detail");
    persistence::WorldSave detailSave(
        "arena-save",
        persistence::WorldIdentity{detailDef->voxel_size_m, detailDef->chunk_size_voxels});
    std::cout << "[main] Save directory: " << detailSave.directory() << "\n";

    // ── LOD + decomposition ───────────────────────────────────────────────────
    LODManager lod(layerConfig);
    // y=-1 catches the foundation slab (500m voxels, chunk at y=-1 covers Y=[-2000,0)).
    // y=0  catches the rampart walls, terrace blocks, and prop columns — all within one
    // 160m/80m/16m chunk in the Y direction. Detail chunks are managed via decomposition
    // only, so their vertical range is effectively unlimited in this band.
    lod.setVerticalBand(-1, 0);

    DecompositionState  decomp;
    DecompositionWorker worker;
    std::cout << "[main] Decomposition worker threads: " << worker.threadCount() << "\n";

    // Mesh stores: one per layer that this demo renders directly.
    MeshStore foundationMeshes;
    MeshStore rampartsMeshes;
    MeshStore terracesMeshes;
    MeshStore propsMeshes;
    MeshStore detailMeshes;     // populated via decomposition results

    // Template voxel captured from the first decomposed terrace, used to restore
    // macro voxels when decomposed detail drifts past kDetailKeepRadiusM.
    Voxel terraceTemplate;
    bool  haveTerraceTemplate = false;

    // ── Apply feature generators to a detail chunk ────────────────────────────
    // Key-spot gold markers are stamped above each platform top surface.
    auto applyFeatures = [&](Chunk& chunk) {
        for (const auto& f : pluginManager.featureGenerators())
            if (f.fn)
                f.fn(chunk.origin(), detail->voxelSizeM(), detail->chunkSizeVoxels(),
                     chunk.data(), f.user_data);
    };

    // ── Detail layer edit helpers ─────────────────────────────────────────────
    // Rebuild the GPU mesh for a detail chunk after an edit.
    auto remeshDetailChunk = [&](const ChunkCoord& cc) {
        const Chunk* chunk = detail->getChunk(cc);
        if (!chunk) return;
        auto it = detailMeshes.find(cc);
        if (it != detailMeshes.end()) {
            it->second.destroy();
            it->second = ChunkMesh::build(*chunk);
        } else {
            detailMeshes.emplace(cc, ChunkMesh::build(*chunk));
        }
    };

    // Write a voxel in the detail layer, fire on_voxel_modified, and re-mesh.
    // No-op if the chunk is not resident (setVoxel returns false).
    auto editDetailVoxel = [&](const chunkmath::VoxelCoord& vc, const Voxel& newVox) {
        const WorldCoord center = chunkmath::voxelCenter(vc, detail->voxelSizeM());
        const Voxel oldVox = world.getVoxel(center);
        if (!world.setVoxel(center, newVox)) return;
        for (const auto& h : pluginManager.voxelModifiedHooks())
            if (h.fn) h.fn(center, &oldVox, &newVox, h.user_data);
        remeshDetailChunk(chunkmath::voxelToChunkLocal(vc, detail->chunkSizeVoxels()).chunk);
    };

    // ── Camera / player state ─────────────────────────────────────────────────
    float      pitch = -0.25f, yaw = 0.0f;
    // Start inside the arena, near the ground, facing the central platform.
    WorldCoord camPos(250.0, 12.0, 80.0);
    double     lastMX = 0.0, lastMY = 0.0;
    bool       firstMouse = true;
    bool       cursorCaptured = true;
    bool       prevKeyF = false, prevKeyG = false;
    bool       prevLeft = false, prevRight = false;

    bool       walkMode = false;
    WorldCoord playerCenter(0.0, 0.0, 0.0);
    double     vy = 0.0;
    bool       grounded = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    std::cout << "[main] Arena platformer. Five-layer world: foundation (500m) + "
                 "ramparts (20m) + terraces (10m) + props (2m) + detail (1m).\n"
                 "[main] WASD + mouse to fly, Space/Shift up/down, G = walk "
                 "(cross-layer collision), F = cursor, ESC quits.\n"
                 "[main] Left mouse = break detail voxel, right mouse = place, "
                 "1-9 = select material.\n"
                 "[main] Fly toward platforms to decompose them; build bridges to "
                 "cross gaps. Edits save to arena-save/ and survive relaunch.\n";

    while (!window.shouldClose()) {
        window.pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // F: toggle cursor capture.
        bool curKeyF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curKeyF && !prevKeyF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                             cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevKeyF = curKeyF;

        // G: toggle walk / fly.
        bool curKeyG = (glfwGetKey(glfwWin, GLFW_KEY_G) == GLFW_PRESS);
        if (curKeyG && !prevKeyG) {
            walkMode = !walkMode;
            if (walkMode) {
                playerCenter = WorldCoord(camPos.value - glm::dvec3(0.0, kEyeOffset, 0.0));
                vy = 0.0; grounded = false;
            }
            std::cout << "[main] Mode: "
                      << (walkMode ? "WALK (gravity + cross-layer collision)" : "FLY")
                      << "\n";
        }
        prevKeyG = curKeyG;

        // Material selection (1-9): choose what the right mouse places.
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
                yaw   += static_cast<float>(mx - lastMX) * kMouseSens;
                pitch -= static_cast<float>(my - lastMY) * kMouseSens;
                if (pitch >  1.55f) pitch =  1.55f;
                if (pitch < -1.55f) pitch = -1.55f;
            }
            lastMX = mx; lastMY = my; firstMouse = false;
        }

        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);

        if (!walkMode) {
            glm::dvec3 fwd  {double(cp * sy), double(sp), double(cp * cy)};
            glm::dvec3 right{double(cy),      0.0,        double(-sy)};
            glm::dvec3 delta{};
            double step = double(kFlySpeed * dt);
            if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += fwd   * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= fwd   * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) delta -= right * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) delta += right * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) delta.y += step;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) delta.y -= step;
            camPos = WorldCoord(camPos.value + delta);
        } else {
            glm::dvec3 fwdH  {double(sy),  0.0, double(cy)};
            glm::dvec3 rightH{double(cy),  0.0, double(-sy)};
            glm::dvec3 wish{};
            if (glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS) wish += fwdH;
            if (glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS) wish -= fwdH;
            if (glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS) wish -= rightH;
            if (glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS) wish += rightH;
            if (glm::length(wish) > 0.0) wish = glm::normalize(wish);
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE) == GLFW_PRESS && grounded)
                vy = kJumpSpeed;
            vy -= kGravity * double(dt);
            glm::dvec3 delta = wish * (kWalkSpeed * double(dt));
            delta.y += vy * double(dt);
            voxelcollide::MoveResult mr =
                voxelcollide::moveAABB(world, {playerCenter, kPlayerHalf}, delta);
            playerCenter = mr.position;
            grounded     = mr.grounded;
            if (mr.grounded || (mr.hitY && vy > 0.0)) vy = 0.0;
            camPos = WorldCoord(playerCenter.value + glm::dvec3(0.0, kEyeOffset, 0.0));
        }

        // ── Stream immutable and composite layers around the camera ───────────
        // Helper: load up to kStreamPerFrame new chunks per frame, evict chunks
        // beyond the LOD budget. Immutable layers are evicted directly; the
        // terraces layer also tears down any decomposed detail children on eviction.
        auto stream = [&](Layer* layer, const std::string& name, MeshStore& meshes,
                          const RegisteredLayerGenerator& gen) {
            const ChunkCoord center =
                chunkmath::worldToChunk(camPos, layer->voxelSizeM(), layer->chunkSizeVoxels());
            int loaded = 0;
            for (const ChunkCoord& c : lod.desiredChunks(center, name)) {
                if (meshes.count(c)) continue;
                Chunk* chunk = layer->loadChunk(c, gen.fn, gen.user_data);
                if (!chunk) continue;
                meshes.emplace(c, ChunkMesh::build(*chunk));
                if (++loaded >= kStreamPerFrame) break;
            }
            const bool isTerraces = (layer == terraces);
            std::vector<ChunkCoord> toEvict;
            for (const auto& kv : meshes)
                if (lod.shouldEvict(center, kv.first, name)) toEvict.push_back(kv.first);
            for (const ChunkCoord& c : toEvict) {
                if (isTerraces) {
                    // Skip eviction if any macro voxel in this chunk has a pending job.
                    const int csz = layer->chunkSizeVoxels();
                    bool anyPending = false;
                    for (int z = 0; z < csz && !anyPending; ++z)
                        for (int y = 0; y < csz && !anyPending; ++y)
                            for (int x = 0; x < csz && !anyPending; ++x)
                                if (decomp.isPending(
                                        chunkmath::chunkLocalToVoxel(c, x, y, z, csz)))
                                    anyPending = true;
                    if (anyPending) continue;
                    // Release decomposed detail children before evicting this chunk.
                    const int n = layer->chunkSizeVoxels();
                    for (int z = 0; z < n; ++z)
                        for (int y = 0; y < n; ++y)
                            for (int x = 0; x < n; ++x) {
                                const chunkmath::VoxelCoord V =
                                    chunkmath::chunkLocalToVoxel(c, x, y, z, n);
                                if (!decomp.isDecomposed(V)) continue;
                                for (const ChunkCoord& dc :
                                         childChunksForMacro(V, *terraces, *detail)) {
                                    auto it = detailMeshes.find(dc);
                                    if (it != detailMeshes.end()) {
                                        it->second.destroy();
                                        detailMeshes.erase(it);
                                    }
                                    // Save dirty detail child before eviction.
                                    if (world.isChunkDirty(dc)) {
                                        if (const Chunk* ch = world.getChunk(dc))
                                            detailSave.saveChunk(*ch);
                                        world.clearChunkDirty(dc);
                                    }
                                    detail->unloadChunk(dc);
                                }
                                decomp.clear(V);
                            }
                }
                meshes[c].destroy();
                meshes.erase(c);
                layer->unloadChunk(c);
            }
        };

        stream(foundation, "foundation", foundationMeshes, foundationGen);
        stream(ramparts,   "ramparts",   rampartsMeshes,   rampartsGen);
        stream(props,      "props",      propsMeshes,       propsGen);
        stream(terraces,   "terraces",   terracesMeshes,   terracesGen);

        // ── Trigger decomposition for nearby terrace macro voxels ─────────────
        int enqueued = 0;
        for (const auto& kv : terracesMeshes) {
            if (enqueued >= kDecomposePerFrame) break;
            const Chunk* chunk = terraces->getChunk(kv.first);
            if (!chunk) continue;
            const int n = chunk->size();
            for (int z = 0; z < n && enqueued < kDecomposePerFrame; ++z)
                for (int y = 0; y < n && enqueued < kDecomposePerFrame; ++y)
                    for (int x = 0; x < n && enqueued < kDecomposePerFrame; ++x) {
                        if (chunk->at(x, y, z).isEmpty()) continue;
                        const chunkmath::VoxelCoord V =
                            chunkmath::chunkLocalToVoxel(kv.first, x, y, z, n);
                        if (!decomp.needsDecompose(V)) continue;
                        const WorldCoord ctr = chunkmath::voxelCenter(V, terraces->voxelSizeM());
                        if (glm::length(ctr.value - camPos.value) > kDecomposeRadiusM) continue;
                        if (!decomp.markPending(V)) continue;
                        DecompositionJob job;
                        job.macro           = V;
                        job.childChunks     = childChunksForMacro(V, *terraces, *detail);
                        job.childChunkSize  = detail->chunkSizeVoxels();
                        job.childVoxelSizeM = detail->voxelSizeM();
                        job.generator       = detailGen.fn;
                        job.userData        = detailGen.user_data;
                        worker.enqueue(job);
                        ++enqueued;
                    }
        }

        // ── Integrate completed decomposition results ─────────────────────────
        // For each generated detail chunk, prefer a player-saved version over the
        // freshly generated one — so player bridges survive across sessions.
        std::unordered_set<ChunkCoord, ChunkCoordHash> terracesToRemesh;
        for (DecompositionResult& result : worker.drain()) {
            for (auto& chunkPtr : result.chunks) {
                const ChunkCoord dc = chunkPtr->coord();
                // Prefer a saved (player-edited) chunk over the generator output.
                bool fromSave = false;
                if (detailSave.hasChunk(dc)) {
                    if (auto saved = detailSave.tryLoadChunk(dc)) {
                        chunkPtr  = std::move(saved);
                        fromSave  = true;
                    }
                }
                Chunk* inserted = detail->insertChunk(std::move(chunkPtr));
                if (!inserted) continue;
                // Apply feature generators only to freshly generated chunks —
                // saved chunks already incorporate the player's edits.
                if (!fromSave) applyFeatures(*inserted);
                auto it = detailMeshes.find(dc);
                if (it != detailMeshes.end()) {
                    it->second.destroy();
                    it->second = ChunkMesh::build(*inserted);
                } else {
                    detailMeshes.emplace(dc, ChunkMesh::build(*inserted));
                }
            }
            // Clear the decomposed macro voxel so it stops rendering and colliding
            // as a coarse block (the fine detail chunks now occupy that space).
            const chunkmath::LocalVoxel lv =
                chunkmath::voxelToChunkLocal(result.macro, terraces->chunkSizeVoxels());
            auto cit = terraces->chunks().find(lv.chunk);
            if (cit != terraces->chunks().end()) {
                if (!haveTerraceTemplate) {
                    terraceTemplate     = cit->second->at(lv.x, lv.y, lv.z);
                    haveTerraceTemplate = true;
                }
                cit->second->at(lv.x, lv.y, lv.z) = Voxel::empty();
                terracesToRemesh.insert(lv.chunk);
            }
            decomp.markDecomposed(result.macro);
        }

        // ── Release detail chunks that have drifted past the keep radius ──────
        // Save dirty chunks before dropping them, then revert the parent terrace
        // macro voxel to its solid block so it still renders/collides and
        // re-decomposes on approach.
        std::vector<ChunkCoord> detailToEvict;
        for (const auto& kv : detailMeshes) {
            // For terraces(10m)/detail(10m chunks), the macro VoxelCoord equals
            // the detail ChunkCoord (same 10m unit).
            const chunkmath::VoxelCoord V{kv.first.x, kv.first.y, kv.first.z};
            const WorldCoord ctr = chunkmath::voxelCenter(V, terraces->voxelSizeM());
            if (glm::length(ctr.value - camPos.value) > kDetailKeepRadiusM)
                detailToEvict.push_back(kv.first);
        }
        for (const ChunkCoord& dc : detailToEvict) {
            // Save-then-evict: write the chunk to disk before dropping it so player
            // edits are never lost when the camera moves away.
            if (world.isChunkDirty(dc)) {
                if (const Chunk* ch = world.getChunk(dc)) detailSave.saveChunk(*ch);
                world.clearChunkDirty(dc);
            }
            auto it = detailMeshes.find(dc);
            if (it != detailMeshes.end()) {
                it->second.destroy();
                detailMeshes.erase(it);
            }
            detail->unloadChunk(dc);
            const chunkmath::VoxelCoord V{dc.x, dc.y, dc.z};
            decomp.clear(V);
            if (haveTerraceTemplate) {
                const chunkmath::LocalVoxel lv =
                    chunkmath::voxelToChunkLocal(V, terraces->chunkSizeVoxels());
                auto cit = terraces->chunks().find(lv.chunk);
                if (cit != terraces->chunks().end()) {
                    cit->second->at(lv.x, lv.y, lv.z) = terraceTemplate;
                    terracesToRemesh.insert(lv.chunk);
                }
            }
        }

        // Re-mesh terrace chunks whose macro voxel occupancy changed.
        for (const ChunkCoord& c : terracesToRemesh) {
            const Chunk* chunk = terraces->getChunk(c);
            if (!chunk) continue;
            auto it = terracesMeshes.find(c);
            if (it != terracesMeshes.end()) {
                it->second.destroy();
                it->second = ChunkMesh::build(*chunk);
            }
        }

        // ── Targeting and edits (detail layer) ───────────────────────────────
        // The DDA raycast targets resident 1 m detail voxels only (primary layer).
        // The player can break and place voxels to bridge gaps or make shortcuts.
        glm::dvec3 lookDir{double(cp * sy), double(sp), double(cp * cy)};
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);
        if (hit.hit)
            renderer.drawVoxelHighlight(
                chunkmath::voxelCenter(hit.voxel, detail->voxelSizeM()),
                static_cast<float>(detail->voxelSizeM()));

        bool curLeft  = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS);
        bool curRight = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        if (hit.hit && curLeft && !prevLeft) {
            editDetailVoxel(hit.voxel, Voxel::empty());  // break
        }
        if (hit.hit && curRight && !prevRight) {
            // Guard against placing into the space the player occupies.
            const double vs = detail->voxelSizeM();
            const chunkmath::VoxelCoord t = hit.adjacent;
            bool blockedByPlayer;
            if (walkMode) {
                glm::dvec3 cmin{double(t.x) * vs, double(t.y) * vs, double(t.z) * vs};
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
                editDetailVoxel(t, placed);
            }
        }
        prevLeft  = curLeft;
        prevRight = curRight;

        // ── Resize ────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        // ── Render every layer at its own voxel scale ─────────────────────────
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        for (const auto& kv : foundationMeshes) {
            const Chunk* c = foundation->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), foundation->voxelSizeM());
        }
        for (const auto& kv : rampartsMeshes) {
            const Chunk* c = ramparts->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), ramparts->voxelSizeM());
        }
        for (const auto& kv : propsMeshes) {
            const Chunk* c = props->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), props->voxelSizeM());
        }
        for (const auto& kv : terracesMeshes) {
            const Chunk* c = terraces->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), terraces->voxelSizeM());
        }
        for (const auto& kv : detailMeshes) {
            const Chunk* c = detail->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), detail->voxelSizeM());
        }

        renderer.render();
    }

    std::cout << "[main] Decomposed terrace macro voxels this session: "
              << decomp.decomposedCount() << "\n";

    // Persist any edited detail chunks still in memory so they survive the next launch.
    int savedOnQuit = detailSave.saveDirtyChunks(world);
    if (savedOnQuit > 0)
        std::cout << "[main] Saved " << savedOnQuit << " edited detail chunk(s) to "
                  << detailSave.directory() << "\n";

    for (auto& kv : foundationMeshes) kv.second.destroy();
    for (auto& kv : rampartsMeshes)   kv.second.destroy();
    for (auto& kv : propsMeshes)      kv.second.destroy();
    for (auto& kv : terracesMeshes)   kv.second.destroy();
    for (auto& kv : detailMeshes)     kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

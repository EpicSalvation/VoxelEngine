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
// Each frame, composite macro voxels within a radius of the camera are enqueued
// on the DecompositionWorker thread pool. When a job returns, its child terrain
// chunk is inserted, the macro voxel is cleared (so the coarse block stops
// rendering and colliding), and the composite chunk is re-meshed — so flying
// toward the blocky horizon refines it into detailed terrain, with pop-in while
// a job is in flight (expected, not a bug; ARCHITECTURE §4).
//
// Controls: WASD move, mouse look, Space/Shift up/down (fly) or jump (walk),
// G toggles walk (gravity + swept AABB collision across all layers), F toggles
// the cursor, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionWorker.h"
#include "world/MacroVoxel.h"
#include "world/VoxelCollision.h"
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

#ifndef VOXEL_LAYERED_PLUGIN_PATH
#  define VOXEL_LAYERED_PLUGIN_PATH ""
#endif

namespace {
constexpr int    kStreamPerFrame    = 2;    // composite/backdrop chunks meshed per frame
constexpr int    kDecomposePerFrame = 48;   // macro voxels enqueued per frame
constexpr double kDecomposeRadiusM  = 144.0; // approach distance that triggers decomposition
// Decomposed terrain is released (reverted to its coarse block) once it drifts
// past this radius, so the resident terrain-mesh count stays bounded. Must be
// > kDecomposeRadiusM (the gap is hysteresis so boundary terrain does not
// thrash) yet small enough that resident terrain chunks stay under the
// renderer's GPU static-buffer ceiling (bgfx caps static vertex/index buffers
// at 4096 each; one per terrain chunk). Without this bound, terrain freed only
// at the far composite view distance piles up past that ceiling while flying —
// new chunks then get invalid GPU buffers and render as invisible-but-collidable
// holes that never recover.
constexpr double kTerrainKeepRadiusM = 160.0;
constexpr float  kFlySpeed          = 32.0f;
constexpr float  kMouseSens         = 0.002f;

constexpr double kWalkSpeed = 6.0;
constexpr double kGravity   = 25.0;
constexpr double kJumpSpeed = 8.0;
constexpr double kEyeOffset = 0.7;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// The child terrain chunks covering one composite macro voxel's subvolume.
// General over the size ratio; for this demo's 8 m → 1 m (8-voxel) terrain it is
// exactly one chunk per macro voxel.
std::vector<ChunkCoord> childChunksForMacro(const chunkmath::VoxelCoord& macro,
                                            const Layer& parent, const Layer& child) {
    const double parentVoxel    = parent.voxelSizeM();
    const double childChunkSize  = child.voxelSizeM() * child.chunkSizeVoxels();
    const int    span = std::max(1, static_cast<int>(std::llround(parentVoxel / childChunkSize)));
    const WorldCoord origin = chunkmath::voxelOrigin(macro, parentVoxel);
    const ChunkCoord base =
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
  - name: backdrop
    voxel_size_m: 2.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 6
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    view_distance_chunks: 6
)");
        } catch (const std::exception& e) {
            std::cerr << "[main] Fatal: layer config error: " << e.what() << "\n";
            std::exit(1);
        }
    }();

    // Materials and the per-layer generators come from the layered-world plugin.
    PluginManager pluginManager;
    if (std::string(VOXEL_LAYERED_PLUGIN_PATH).empty()) {
        std::cerr << "[main] Fatal: layered-world plugin path not configured at build time.\n";
        return 1;
    }
    if (pluginManager.loadPlugin(VOXEL_LAYERED_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load layered-world plugin from "
                  << VOXEL_LAYERED_PLUGIN_PATH << "\n";
        return 1;
    }

    auto findGenerator = [&](const std::string& name) -> RegisteredLayerGenerator {
        for (const auto& g : pluginManager.layerGenerators())
            if (g.layer_name == name) return g;
        return RegisteredLayerGenerator{name, nullptr, nullptr, kInvalidPluginId};
    };
    const RegisteredLayerGenerator blocksGen   = findGenerator("blocks");
    const RegisteredLayerGenerator backdropGen = findGenerator("backdrop");
    const RegisteredLayerGenerator terrainGen  = findGenerator("terrain");
    if (!blocksGen.fn || !backdropGen.fn || !terrainGen.fn) {
        std::cerr << "[main] Fatal: layered-world plugin did not register all three layer "
                     "generators (blocks, backdrop, terrain).\n";
        return 1;
    }

    Engine engine;
    engine.start();

    platform::Window window(1024, 768, "VoxelEngine — M6 Decompose on Approach");

    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    World world(layerConfig);
    Layer* blocks   = world.layer("blocks");
    Layer* backdrop = world.layer("backdrop");
    Layer* terrain  = world.layer("terrain");
    if (!blocks || !backdrop || !terrain) {
        std::cerr << "[main] Fatal: expected blocks/backdrop/terrain layers.\n";
        return 1;
    }

    LODManager lod(layerConfig);
    // Composite surface lives in chunk-Y 0; the immutable bedrock slab sits in
    // the backdrop's chunk-Y -1. One band covers both (empty chunks are cheap).
    lod.setVerticalBand(-1, 0);

    DecompositionState   decomp;
    DecompositionWorker  worker;  // hardware-sized thread pool

    // A coarse block voxel, captured the first time one is decomposed, so terrain
    // that leaves the keep radius can be reverted to its block (see below).
    Voxel blockTemplate;
    bool  haveBlockTemplate = false;
    std::cout << "[main] Decomposition worker threads: " << worker.threadCount() << "\n";

    MeshStore blocksMeshes;    // composite atomic blocks (rendered at 8 m)
    MeshStore backdropMeshes;  // immutable bedrock (rendered at 2 m)
    MeshStore terrainMeshes;   // decomposed fine terrain (rendered at 1 m)

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

    std::cout << "[main] Fly toward the blocky terrain to decompose it. WASD + mouse, "
                 "Space/Shift up/down,\n"
                 "[main] G = walk (collision), F = cursor, ESC quits.\n";

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
            std::cout << "[main] Mode: " << (walkMode ? "WALK (gravity + collision)" : "FLY")
                      << "\n";
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

        // ── Stream the composite and immutable layers around the camera ───
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
            // Evict chunks beyond the view budget. The immutable backdrop just
            // drops (no dirty/persist); the composite layer also releases the
            // decomposed terrain children it owns.
            const bool composite = (layer == blocks);
            std::vector<ChunkCoord> toEvict;
            for (const auto& kv : meshes)
                if (lod.shouldEvict(center, kv.first, name)) toEvict.push_back(kv.first);
            for (const ChunkCoord& c : toEvict) {
                if (composite) {
                    const int n = layer->chunkSizeVoxels();
                    bool anyPending = false;
                    for (int z = 0; z < n && !anyPending; ++z)
                        for (int y = 0; y < n && !anyPending; ++y)
                            for (int x = 0; x < n && !anyPending; ++x)
                                if (decomp.isPending(chunkmath::chunkLocalToVoxel(c, x, y, z, n)))
                                    anyPending = true;
                    if (anyPending) continue;  // a job is in flight; evict later
                    for (int z = 0; z < n; ++z)
                        for (int y = 0; y < n; ++y)
                            for (int x = 0; x < n; ++x) {
                                const chunkmath::VoxelCoord V =
                                    chunkmath::chunkLocalToVoxel(c, x, y, z, n);
                                if (!decomp.isDecomposed(V)) continue;
                                for (const ChunkCoord& tcc :
                                     childChunksForMacro(V, *blocks, *terrain)) {
                                    auto it = terrainMeshes.find(tcc);
                                    if (it != terrainMeshes.end()) {
                                        it->second.destroy();
                                        terrainMeshes.erase(it);
                                    }
                                    terrain->unloadChunk(tcc);
                                }
                                decomp.clear(V);
                            }
                }
                meshes[c].destroy();
                meshes.erase(c);
                layer->unloadChunk(c);
            }
        };
        stream(blocks,   "blocks",   blocksMeshes,   blocksGen);
        stream(backdrop, "backdrop", backdropMeshes, backdropGen);

        // ── Trigger decomposition for nearby composite macro voxels ───────
        int enqueued = 0;
        for (const auto& kv : blocksMeshes) {
            if (enqueued >= kDecomposePerFrame) break;
            const Chunk* chunk = blocks->getChunk(kv.first);
            if (!chunk) continue;
            const int n = chunk->size();
            for (int z = 0; z < n && enqueued < kDecomposePerFrame; ++z)
                for (int y = 0; y < n && enqueued < kDecomposePerFrame; ++y)
                    for (int x = 0; x < n && enqueued < kDecomposePerFrame; ++x) {
                        if (chunk->at(x, y, z).isEmpty()) continue;
                        const chunkmath::VoxelCoord V =
                            chunkmath::chunkLocalToVoxel(kv.first, x, y, z, n);
                        if (!decomp.needsDecompose(V)) continue;
                        const WorldCoord ctr =
                            chunkmath::voxelCenter(V, blocks->voxelSizeM());
                        if (glm::length(ctr.value - camPos.value) > kDecomposeRadiusM) continue;
                        if (!decomp.markPending(V)) continue;
                        DecompositionJob job;
                        job.macro           = V;
                        job.childChunks     = childChunksForMacro(V, *blocks, *terrain);
                        job.childChunkSize  = terrain->chunkSizeVoxels();
                        job.childVoxelSizeM = terrain->voxelSizeM();
                        job.generator       = terrainGen.fn;
                        job.userData        = terrainGen.user_data;
                        worker.enqueue(job);
                        ++enqueued;
                    }
        }

        // ── Integrate completed decomposition jobs ────────────────────────
        std::unordered_set<ChunkCoord, ChunkCoordHash> compositeToRemesh;
        for (DecompositionResult& result : worker.drain()) {
            for (auto& chunkPtr : result.chunks) {
                const ChunkCoord tcc = chunkPtr->coord();
                Chunk* inserted = terrain->insertChunk(std::move(chunkPtr));
                if (!inserted) continue;
                auto it = terrainMeshes.find(tcc);
                if (it != terrainMeshes.end()) {
                    it->second.destroy();
                    it->second = ChunkMesh::build(*inserted);
                } else {
                    terrainMeshes.emplace(tcc, ChunkMesh::build(*inserted));
                }
            }
            // Clear the now-decomposed macro voxel so the coarse block stops
            // rendering and contributing to collision.
            const chunkmath::LocalVoxel lv =
                chunkmath::voxelToChunkLocal(result.macro, blocks->chunkSizeVoxels());
            auto cit = blocks->chunks().find(lv.chunk);
            if (cit != blocks->chunks().end()) {
                if (!haveBlockTemplate) {
                    blockTemplate     = cit->second->at(lv.x, lv.y, lv.z);
                    haveBlockTemplate = true;
                }
                cit->second->at(lv.x, lv.y, lv.z) = Voxel::empty();
                compositeToRemesh.insert(lv.chunk);
            }
            decomp.markDecomposed(result.macro);
        }

        // ── Release decomposed terrain that has drifted past the keep radius ──
        // Revert it to its coarse block so the macro voxel still renders and
        // collides (as a block) and re-decomposes when the camera returns. This
        // bounds the resident terrain-mesh count; see kTerrainKeepRadiusM.
        // For this demo's 8 m → 1 m stack a macro voxel and its terrain chunk
        // share one coord, so the macro VoxelCoord is just the terrain chunk's.
        std::vector<ChunkCoord> terrainToEvict;
        for (const auto& kv : terrainMeshes) {
            const chunkmath::VoxelCoord V{kv.first.x, kv.first.y, kv.first.z};
            const WorldCoord ctr = chunkmath::voxelCenter(V, blocks->voxelSizeM());
            if (glm::length(ctr.value - camPos.value) > kTerrainKeepRadiusM)
                terrainToEvict.push_back(kv.first);
        }
        for (const ChunkCoord& tcc : terrainToEvict) {
            auto it = terrainMeshes.find(tcc);
            if (it != terrainMeshes.end()) {
                it->second.destroy();
                terrainMeshes.erase(it);
            }
            terrain->unloadChunk(tcc);
            const chunkmath::VoxelCoord V{tcc.x, tcc.y, tcc.z};
            decomp.clear(V);
            if (haveBlockTemplate) {
                const chunkmath::LocalVoxel lv =
                    chunkmath::voxelToChunkLocal(V, blocks->chunkSizeVoxels());
                auto cit = blocks->chunks().find(lv.chunk);
                if (cit != blocks->chunks().end()) {
                    cit->second->at(lv.x, lv.y, lv.z) = blockTemplate;
                    compositeToRemesh.insert(lv.chunk);
                }
            }
        }

        for (const ChunkCoord& c : compositeToRemesh) {
            const Chunk* chunk = blocks->getChunk(c);
            if (!chunk) continue;
            auto it = blocksMeshes.find(c);
            if (it != blocksMeshes.end()) {
                it->second.destroy();
                it->second = ChunkMesh::build(*chunk);
            }
        }

        // Resize.
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        // ── Render every layer at its own voxel scale ─────────────────────
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        for (const auto& kv : backdropMeshes) {
            const Chunk* chunk = backdrop->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), backdrop->voxelSizeM(), backdrop->chunkSizeVoxels());
        }
        for (const auto& kv : blocksMeshes) {
            const Chunk* chunk = blocks->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), blocks->voxelSizeM(), blocks->chunkSizeVoxels());
        }
        for (const auto& kv : terrainMeshes) {
            const Chunk* chunk = terrain->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), terrain->voxelSizeM(), terrain->chunkSizeVoxels());
        }
        renderer.render();
    }

    std::cout << "[main] Decomposed macro voxels this session: " << decomp.decomposedCount()
              << "\n";

    for (auto& kv : blocksMeshes)   kv.second.destroy();
    for (auto& kv : backdropMeshes) kv.second.destroy();
    for (auto& kv : terrainMeshes)  kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

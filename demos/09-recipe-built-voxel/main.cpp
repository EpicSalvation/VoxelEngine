// Demo 09 — recipe-built voxel.
//
// The composite-RECIPE rung of the ladder. Where demo 05 decomposed a macro voxel
// by re-running a child generator, this world attaches a declarative composition
// recipe to its composite "blocks" layer (the recipe-world plugin), and the
// engine-owned DecompositionManager resolves and applies it. A recipe describes
// HOW a coarse macro voxel fills itself when it decomposes (ARCHITECTURE §6):
//
//   1. interior  — a granite/basalt material distribution arranged by a noise field
//   2. top cap   — a 2-voxel SOIL boundary on the macro's gravity-opposing face
//   3. caves     — a feature overlay carving connected voids
//   4. ore veins — a feature overlay threading iron through the granite bulk
//
// Two things this teaches beyond demo 05:
//   * Declarative composition. The fine interior is not hand-written by a
//     generator; it is COMPOSED from a recipe's distribution + boundary + ordered
//     feature overlays. Dig into a decomposed block to see the result in section:
//     soil cap on top, stone bulk, cave voids, ore veins.
//   * A cascaded seed parameter shaping the recipe SPATIALLY. The cave feature
//     reads the engine-cascaded "__altitude" (the decomposing macro's height,
//     injected into the recipe's effective params by the manager — §6/§10), so
//     caves thicken with depth. Dig a shaft straight down: sparse caves near the
//     surface give way to swiss-cheese near bedrock. It is fully deterministic —
//     fly away so the fine terrain cascade-collapses back to coarse blocks, return,
//     and every block regenerates byte-for-byte (no runtime state, just position).
//
// Everything streams through the DecompositionManager: it decomposes "blocks" on
// approach, streams the immutable "bedrock" floor under its own volume/budget, and
// collapses out-of-range fine terrain so the resident mesh count stays bounded.
//
// Controls: WASD move, mouse look, Space/Shift up/down (fly) or jump (walk),
// G walk (gravity + collision), LEFT-CLICK mine, RIGHT-CLICK place soil,
// F cursor, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "core/RecipeValidation.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "world/Chunk.h"
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
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VOXEL_RECIPE_PLUGIN_PATH
#  define VOXEL_RECIPE_PLUGIN_PATH ""
#endif

namespace {
constexpr char kLogCat[] = "demo09";

// Decompose composite macro voxels within this range of the camera. The manager
// streams the "blocks" root out to its view distance and decomposes only inside
// this bubble; the per-layer resident_chunk_budget collapses the farthest fine
// terrain back to coarse blocks, keeping the resident mesh count under bgfx's
// 4096 static-buffer ceiling (ARCHITECTURE §10).
constexpr double kApproachRadiusM = 64.0;

constexpr double kReachM    = 8.0;     // voxel-pick reach (metres)
constexpr float  kToolPower = 5.0f;    // dig speed (units/s)
constexpr float  kFlySpeed  = 28.0f;
constexpr float  kMouseSens = 0.002f;

constexpr double kWalkSpeed = 6.0;
constexpr double kGravity   = 25.0;
constexpr double kJumpSpeed = 8.0;
constexpr double kEyeOffset = 0.7;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

// World seed for decomposition determinism. The per-decomposition seed folds this
// with the macro VoxelCoord, so each macro decomposes the same way every time.
constexpr uint64_t kWorldSeed = 0x9E3779B97F4A7C15ull;

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// Replace (or create) a chunk mesh, destroying old GPU buffers first (plain
// assignment leaks bgfx handles into the capped static-buffer pool). All-air
// chunks hold no buffers and are kept OUT of the store.
void rebuildMesh(MeshStore& ms, const ChunkCoord& cc, const Chunk& chunk) {
    auto it = ms.find(cc);
    if (it != ms.end()) { it->second.destroy(); ms.erase(it); }
    ChunkMesh mesh = ChunkMesh::build(chunk);
    if (mesh.empty()) return;
    ms.emplace(cc, std::move(mesh));
}

// Apply one DecompositionManager::tick() diff set to the per-layer mesh stores
// (the M10 front-end contract; identical in shape to demo 10's applyDiffs). The
// manager owns all decomposition/streaming state and only reports which meshes to
// build or destroy. Immutable-layer diffs (the bedrock floor) arrive with the
// layer name in compositeLayerName and their load/evict in newCompChunks/evicted.
void applyDiffs(const std::vector<LayerTickDiff>& diffs, World& world,
                std::unordered_map<std::string, MeshStore>& meshStores) {
    for (const LayerTickDiff& d : diffs) {
        MeshStore& compMeshes = meshStores[d.compositeLayerName];
        Layer* compLayer  = world.layer(d.compositeLayerName);
        Layer* childLayer = d.childLayerName.empty() ? nullptr
                                                     : world.layer(d.childLayerName);

        for (const ChunkCoord& cc : d.newCompChunks) {
            if (!compLayer) continue;
            if (const Chunk* chunk = compLayer->getChunk(cc)) rebuildMesh(compMeshes, cc, *chunk);
        }
        for (const ChunkCoord& cc : d.evictedCompChunks) {
            auto it = compMeshes.find(cc);
            if (it != compMeshes.end()) { it->second.destroy(); compMeshes.erase(it); }
        }

        // Macro voxels whose block state changed: remesh the owning composite chunk
        // (newly decomposed cleared its block; re-atomized restored it). One terrain
        // chunk per macro here, so the owning-chunk lookup is the dedupe.
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
    // Coarsest-first. blocks(8 m)→bedrock(2 m)→terrain(1 m) satisfies the adjacent
    // strictly-descending integer-ratio rule (8/2=4, 2/1=2). Parent voxel (8 m) ==
    // terrain chunk world size (1 m × 8), so each macro decomposes into exactly one
    // terrain chunk. resident_chunk_budget bounds resident meshes (M16 L5).
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
  - name: bedrock
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

    // ── Plugin loading ──────────────────────────────────────────────────────────
    if (std::string(VOXEL_RECIPE_PLUGIN_PATH).empty()) {
        Log::error(kLogCat, "Fatal: recipe-world plugin path not configured at build time.");
        return 1;
    }

    World world(layerConfig);
    // Engine::init registers the built-in "value" noise (the recipe's interior and
    // cave features use it) and the .vox handlers.
    Engine engine;
    PluginManager pluginManager;
    engine.init(pluginManager, world);

    if (pluginManager.loadPlugin(VOXEL_RECIPE_PLUGIN_PATH) == kInvalidPluginId) {
        Log::error(kLogCat, (std::string("Fatal: could not load recipe-world plugin from ")
                             + VOXEL_RECIPE_PLUGIN_PATH).c_str());
        return 1;
    }

    // Startup validation: the composite layer's recipe must resolve to registered
    // feature/noise ids (a hard error, not a silent skip).
    try {
        validateRecipes(layerConfig, pluginManager);
    } catch (const std::exception& e) {
        Log::error(kLogCat, (std::string("Fatal: recipe validation failed: ") + e.what()).c_str());
        return 1;
    }

    Layer* blocks  = world.layer("blocks");
    Layer* terrain = world.layer("terrain");
    Layer* bedrock = world.layer("bedrock");
    if (!blocks || !terrain || !bedrock) {
        Log::error(kLogCat, "Fatal: expected blocks/terrain/bedrock layers.");
        return 1;
    }

    // ── Engine-owned cascade orchestrator ───────────────────────────────────────
    // Resolves and applies the "blocks" recipe on decomposition, streams the
    // immutable "bedrock" floor under its own volume/budget, and collapses
    // out-of-range terrain. The ground slab fills blocks chunk-Y 0 (a 64 m row);
    // band the generator-streamed root there so empty sky/underground is skipped.
    DecompositionManager decompMgr(world, pluginManager, layerConfig, kWorldSeed);
    decompMgr.setVerticalBand(0, 0);

    platform::Window window(1024, 768, "VoxelEngine — Recipe-Built Voxel");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);
    engine.setRenderer(&renderer);
    engine.setDecompositionManager(&decompMgr);

    // One MeshStore per layer.
    std::unordered_map<std::string, MeshStore> meshStores;

    // Remesh a single terrain chunk after a voxel edit.
    auto remeshTerrainChunk = [&](const chunkmath::VoxelCoord& vc) {
        const chunkmath::LocalVoxel lv =
            chunkmath::voxelToChunkLocal(vc, terrain->chunkSizeVoxels());
        const Chunk* chunk = terrain->getChunk(lv.chunk);
        if (chunk) rebuildMesh(meshStores["terrain"], lv.chunk, *chunk);
    };

    sim::RemovalAccumulator remover;
    bool prevRight = false;

    // Camera.
    float      pitch = -0.35f, yaw = 0.0f;
    WorldCoord camPos(0.0, 56.0, -16.0);  // above the 48 m slab top, looking in
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true, cursorCaptured = true;
    bool       prevKeyF = false, prevKeyG = false;

    bool       walkMode = false;
    WorldCoord playerCenter(0.0, 0.0, 0.0);
    double     vy = 0.0;
    bool       grounded = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    Log::info(kLogCat, "Fly toward the blocks to decompose them via the recipe. "
                       "LEFT-CLICK mines, RIGHT-CLICK places soil.");
    Log::info(kLogCat, "Dig a shaft straight down: caves thicken with depth "
                       "(the cascaded __altitude param). G = walk, F = cursor, ESC quits.");

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
                vy = 0.0; grounded = false;
            }
            Log::info(kLogCat, walkMode ? "Mode: WALK" : "Mode: FLY");
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
            lastMouseX = mx; lastMouseY = my; firstMouse = false;
        }

        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);
        const glm::dvec3 look{static_cast<double>(cp * sy), static_cast<double>(sp),
                              static_cast<double>(cp * cy)};

        if (!walkMode) {
            glm::dvec3 right{static_cast<double>(cy), 0.0, static_cast<double>(-sy)};
            glm::dvec3 delta{0.0, 0.0, 0.0};
            double step = static_cast<double>(kFlySpeed * dt);
            if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += look  * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= look  * step;
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
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE) == GLFW_PRESS && grounded) vy = kJumpSpeed;
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
        // Resolves+applies the recipe for in-range "blocks" macros, streams the
        // immutable bedrock, and collapses out-of-range terrain. The diffs say
        // which meshes to build/destroy; applyPerFrame bounds per-frame mesh builds.
        auto diffs = decompMgr.tick(camPos, kApproachRadiusM,
                                    /*loadPerFrame=*/32, /*decompPerFrame=*/128,
                                    /*applyPerFrame=*/16);
        applyDiffs(diffs, world, meshStores);

        // Terminal terrain is populated entirely by decomposition; guard against
        // any mesh whose chunk the cascade has since removed.
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

        // ── Terrain editing — dig into a decomposed block to read the recipe ─────
        {
            voxelcast::RayHit hit = voxelcast::raycast(world, camPos, look, kReachM);
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
                placed.material = pluginManager.material("soil");
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

        // ── HUD ─────────────────────────────────────────────────────────────────
        {
            const auto metrics = engine.getMetrics();
            int blockDecomp = 0, terrChunks = 0;
            for (const auto& lm : metrics.layers) {
                if (lm.layerName == "blocks") blockDecomp = static_cast<int>(lm.decomposedMacros);
                else if (lm.layerName == "terrain") terrChunks = static_cast<int>(lm.residentChunks);
            }
            char hud[256];
            std::snprintf(hud, sizeof(hud),
                "recipe: granite/basalt + soil cap + caves(deeper=more) + ore | "
                "decomposed=%d terrain=%d y=%.0f | LMB mine RMB soil",
                blockDecomp, terrChunks, camPos.value.y);
            renderer.setHudText({std::string(hud)});
        }

        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        // ── Render every layer at its own voxel scale (coarsest first) ───────────
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        for (const auto& layerName : std::vector<std::string>{"bedrock", "blocks", "terrain"}) {
            Layer* lyr = world.layer(layerName);
            if (!lyr) continue;
            const MeshStore& ms = meshStores[layerName];
            for (const auto& kv : ms) {
                const Chunk* chunk = lyr->getChunk(kv.first);
                if (!chunk || kv.second.empty()) continue;
                renderer.renderChunk(kv.second, chunk->origin(), lyr->voxelSizeM());
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

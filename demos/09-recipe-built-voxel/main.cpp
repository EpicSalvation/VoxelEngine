// M9 demo — recipe-built voxel.
//
// A three-layer world driven by a composition RECIPE (the recipe-world plugin):
//
//   - "blocks"  — a composite layer of 8 m macro voxels forming a solid ground
//                 slab. Rendered as atomic gray blocks until decomposed.
//   - "terrain" — the fine 1 m child layer the macro voxels decompose INTO. Its
//                 voxels are produced by the recipe, NOT a plain generator: a
//                 granite/basalt material distribution, a two-voxel soil cap on
//                 each macro voxel's top face, then a cave-network overlay and an
//                 ore-vein overlay (in that order).
//   - "bedrock" — an immutable 2 m floor slab beneath the world. It never
//                 decomposes; it only catches a player who walks into a cave or
//                 falls through the bottom of the terrain, so nobody drops into
//                 the void.
//
// Decomposition is recipe-driven: each job carries the resolved recipe plus a
// deterministic seed from (world_seed, macro VoxelCoord), so an evicted macro
// regenerates byte-for-byte on revisit. Trigger it two ways — fly within range
// of a block (approach), or LEFT-CLICK a block (composite picking, the M6
// deferral). Press T to toggle the parent seed parameter "cave_density" between
// two values: the world re-decomposes with visibly different cave density, and
// each value regenerates identically when you return to it.
//
// Controls: WASD move, mouse look, Space/Shift up/down (fly) or jump (walk),
// G walk (gravity + collision), LEFT-CLICK decompose the targeted block,
// T toggle cave density, F cursor, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/RecipeValidation.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionWorker.h"
#include "world/MacroVoxel.h"
#include "world/Recipe.h"
#include "world/RecipeResolve.h"
#include "world/ResolvedRecipe.h"
#include "world/VoxelCollision.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef VOXEL_RECIPE_PLUGIN_PATH
#  define VOXEL_RECIPE_PLUGIN_PATH ""
#endif

namespace {
constexpr int    kStreamPerFrame    = 2;
constexpr int    kDecomposePerFrame = 48;
constexpr double kDecomposeRadiusM  = 120.0;
constexpr double kTerrainKeepRadiusM = 136.0;  // > decompose radius (hysteresis); bounds resident terrain
constexpr float  kFlySpeed          = 28.0f;
constexpr float  kMouseSens         = 0.002f;
constexpr double kPickReachM        = 96.0;    // composite-picking ray reach

constexpr double kWalkSpeed = 6.0;
constexpr double kGravity   = 25.0;
constexpr double kJumpSpeed = 8.0;
constexpr double kEyeOffset = 0.7;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

// World seed for decomposition determinism. The per-decomposition seed folds
// this with the macro VoxelCoord, so each macro voxel decomposes the same way
// every time (and differently from its neighbors).
constexpr uint64_t kWorldSeed = 0x9E3779B97F4A7C15ull;

// The two parent seed-parameter values the T key toggles between.
constexpr double kCaveDensityLow  = 0.20;
constexpr double kCaveDensityHigh = 0.55;

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

uint64_t macroSeed(const chunkmath::VoxelCoord& macro) {
    return voxel_seed_mix(kWorldSeed, chunkmath::VoxelCoordHash{}(macro));
}

// The child terrain chunks covering one composite macro voxel's subvolume.
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
  - name: bedrock
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

    PluginManager pluginManager;
    if (std::string(VOXEL_RECIPE_PLUGIN_PATH).empty()) {
        std::cerr << "[main] Fatal: recipe-world plugin path not configured at build time.\n";
        return 1;
    }

    World world(layerConfig);

    // Engine::init registers the built-in noise floor (the recipe's interior uses
    // the built-in "value" field) and the .vox handlers.
    Engine engine;
    engine.init(pluginManager, world);

    if (pluginManager.loadPlugin(VOXEL_RECIPE_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load recipe-world plugin from "
                  << VOXEL_RECIPE_PLUGIN_PATH << "\n";
        return 1;
    }

    // Startup validation: every composite layer must resolve to a recipe whose
    // feature/noise ids are all registered (a hard error, not a silent skip).
    try {
        validateRecipes(layerConfig, pluginManager);
    } catch (const std::exception& e) {
        std::cerr << "[main] Fatal: recipe validation failed: " << e.what() << "\n";
        return 1;
    }

    auto findGenerator = [&](const std::string& name) {
        for (const auto& g : pluginManager.layerGenerators())
            if (g.layer_name == name) return g;
        return RegisteredLayerGenerator{name, nullptr, nullptr, kInvalidPluginId};
    };
    const RegisteredLayerGenerator blocksGen  = findGenerator("blocks");
    const RegisteredLayerGenerator bedrockGen = findGenerator("bedrock");
    const Recipe* baseRecipe = pluginManager.findRecipe("blocks");
    if (!blocksGen.fn || !bedrockGen.fn || !baseRecipe) {
        std::cerr << "[main] Fatal: recipe-world plugin did not register the blocks "
                     "and bedrock generators plus the blocks recipe.\n";
        return 1;
    }

    // Resolve the recipe for a given cave_density (the parent seed parameter the
    // T key toggles). Re-resolving threads the new value through the effective
    // param set handed to the cave generator (see RecipeResolve / ARCHITECTURE §6).
    auto resolveForDensity = [&](double caveDensity) {
        Recipe r = *baseRecipe;
        bool found = false;
        for (RecipeParamValue& p : r.seed_parameters)
            if (p.key == "cave_density") { p.number = caveDensity; found = true; }
        if (!found) {
            RecipeParamValue p;
            p.key = "cave_density"; p.kind = RecipeParamKind::Number; p.number = caveDensity;
            r.seed_parameters.push_back(p);
        }
        return std::make_shared<const ResolvedRecipe>(resolveRecipe(r, pluginManager));
    };

    double caveDensity = kCaveDensityLow;
    std::shared_ptr<const ResolvedRecipe> resolved = resolveForDensity(caveDensity);
    const int64_t ratio = chunkmath::layerRatio(8.0, 1.0);  // 8

    platform::Window window(1024, 768, "VoxelEngine — M9 Recipe-Built Voxel");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    Layer* blocks  = world.layer("blocks");
    Layer* terrain = world.layer("terrain");
    Layer* bedrock = world.layer("bedrock");
    if (!blocks || !terrain || !bedrock) {
        std::cerr << "[main] Fatal: expected blocks/terrain/bedrock layers.\n";
        return 1;
    }

    LODManager lod(layerConfig);
    // The ground slab lives in chunk-Y 0; the immutable bedrock floor (world
    // y in [-6,0), 2 m voxels / 16 m chunks) sits in chunk-Y -1. One band covers
    // both (empty chunks are cheap).
    lod.setVerticalBand(-1, 0);

    DecompositionState  decomp;
    DecompositionWorker worker;
    std::cout << "[main] Decomposition worker threads: " << worker.threadCount() << "\n";

    Voxel blockTemplate;
    bool  haveBlockTemplate = false;

    MeshStore blocksMeshes;
    MeshStore terrainMeshes;
    MeshStore bedrockMeshes;  // immutable floor (rendered at 2 m, never decomposed)

    // Build a recipe-driven decomposition job for one macro voxel.
    auto makeJob = [&](const chunkmath::VoxelCoord& macro) {
        DecompositionJob job;
        job.macro           = macro;
        job.childChunks     = childChunksForMacro(macro, *blocks, *terrain);
        job.childChunkSize  = terrain->chunkSizeVoxels();
        job.childVoxelSizeM = terrain->voxelSizeM();
        job.recipe          = resolved;
        job.seed            = macroSeed(macro);
        job.ratio           = ratio;
        job.macroChildMin   = chunkmath::childVoxelMin(macro, ratio);
        return job;
    };

    // Camera.
    float      pitch = -0.35f, yaw = 0.0f;
    WorldCoord camPos(0.0, 40.0, -52.0);
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true, cursorCaptured = true;
    bool       prevKeyF = false, prevKeyG = false, prevKeyT = false, prevMouseL = false;

    bool       walkMode = false;
    WorldCoord playerCenter(0.0, 0.0, 0.0);
    double     vy = 0.0;
    bool       grounded = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    std::cout << "[main] Fly toward the blocks (or LEFT-CLICK one) to decompose. "
                 "T toggles cave density,\n"
                 "[main] G = walk, F = cursor, ESC quits. cave_density = "
              << caveDensity << "\n";

    // Drop every decomposed macro back to its atomic block (used on a density
    // toggle, so the whole world re-decomposes under the new recipe). Drains any
    // in-flight jobs first so a late result cannot resurrect stale terrain after
    // the reset.
    auto dropAllTerrain = [&]() {
        while (worker.inFlight() > 0) worker.drain();
        worker.drain();
        for (auto& kv : terrainMeshes) kv.second.destroy();
        terrainMeshes.clear();
        std::vector<ChunkCoord> tcs;
        for (const auto& kv : terrain->chunks()) tcs.push_back(kv.first);
        for (const ChunkCoord& c : tcs) terrain->unloadChunk(c);
        decomp = DecompositionState{};
        // Re-generate every resident blocks chunk so any macro voxel cleared on
        // decompose returns to its atomic block, then re-mesh it.
        std::vector<ChunkCoord> bcs;
        for (const auto& kv : blocksMeshes) bcs.push_back(kv.first);
        for (const ChunkCoord& c : bcs) {
            blocks->unloadChunk(c);
            Chunk* chunk = blocks->loadChunk(c, blocksGen.fn, blocksGen.user_data);
            auto it = blocksMeshes.find(c);
            if (it != blocksMeshes.end()) { it->second.destroy(); it->second = ChunkMesh::build(*chunk); }
        }
    };

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
            std::cout << "[main] Mode: " << (walkMode ? "WALK" : "FLY") << "\n";
        }
        prevKeyG = curKeyG;

        bool curKeyT = (glfwGetKey(glfwWin, GLFW_KEY_T) == GLFW_PRESS);
        if (curKeyT && !prevKeyT) {
            caveDensity = (caveDensity == kCaveDensityLow) ? kCaveDensityHigh : kCaveDensityLow;
            resolved = resolveForDensity(caveDensity);
            dropAllTerrain();
            std::cout << "[main] cave_density = " << caveDensity
                      << " (re-decomposing)\n";
        }
        prevKeyT = curKeyT;

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

        // ── Composite picking: LEFT-CLICK decomposes the targeted block ───────
        bool curMouseL = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        if (curMouseL && !prevMouseL && cursorCaptured) {
            // March the look ray through the blocks layer (8 m voxels). The first
            // solid, not-yet-decomposed macro voxel hit is enqueued for recipe
            // decomposition — interacting with a block triggers its recipe (§6).
            const double bvs = blocks->voxelSizeM();
            const double stepM = bvs * 0.25;
            for (double t = 0.0; t <= kPickReachM; t += stepM) {
                const WorldCoord p(camPos.value + look * t);
                if (blocks->getVoxel(p).isEmpty()) continue;
                const chunkmath::VoxelCoord macro = chunkmath::worldToVoxel(p, bvs);
                if (decomp.needsDecompose(macro) && decomp.markPending(macro))
                    worker.enqueue(makeJob(macro));
                break;
            }
        }
        prevMouseL = curMouseL;

        // ── Stream the composite layer around the camera ──────────────────────
        {
            const ChunkCoord center =
                chunkmath::worldToChunk(camPos, blocks->voxelSizeM(), blocks->chunkSizeVoxels());
            int loaded = 0;
            for (const ChunkCoord& c : lod.desiredChunks(center, "blocks")) {
                if (blocksMeshes.count(c)) continue;
                Chunk* chunk = blocks->loadChunk(c, blocksGen.fn, blocksGen.user_data);
                if (!chunk) continue;
                blocksMeshes.emplace(c, ChunkMesh::build(*chunk));
                if (++loaded >= kStreamPerFrame) break;
            }
            std::vector<ChunkCoord> toEvict;
            for (const auto& kv : blocksMeshes)
                if (lod.shouldEvict(center, kv.first, "blocks")) toEvict.push_back(kv.first);
            for (const ChunkCoord& c : toEvict) {
                const int n = blocks->chunkSizeVoxels();
                bool anyPending = false;
                for (int z = 0; z < n && !anyPending; ++z)
                    for (int y = 0; y < n && !anyPending; ++y)
                        for (int x = 0; x < n && !anyPending; ++x)
                            if (decomp.isPending(chunkmath::chunkLocalToVoxel(c, x, y, z, n)))
                                anyPending = true;
                if (anyPending) continue;
                for (int z = 0; z < n; ++z)
                    for (int y = 0; y < n; ++y)
                        for (int x = 0; x < n; ++x) {
                            const chunkmath::VoxelCoord V = chunkmath::chunkLocalToVoxel(c, x, y, z, n);
                            if (!decomp.isDecomposed(V)) continue;
                            for (const ChunkCoord& tcc : childChunksForMacro(V, *blocks, *terrain)) {
                                auto it = terrainMeshes.find(tcc);
                                if (it != terrainMeshes.end()) { it->second.destroy(); terrainMeshes.erase(it); }
                                terrain->unloadChunk(tcc);
                            }
                            decomp.clear(V);
                        }
                blocksMeshes[c].destroy();
                blocksMeshes.erase(c);
                blocks->unloadChunk(c);
            }
        }

        // ── Stream the immutable bedrock floor around the camera ──────────────
        // Generated once and retained; it never decomposes, so eviction just
        // drops it. It collides via World::anySolidAt, so a player who digs or
        // falls through the bottom of the terrain lands on it instead of the void.
        {
            const ChunkCoord center =
                chunkmath::worldToChunk(camPos, bedrock->voxelSizeM(), bedrock->chunkSizeVoxels());
            int loaded = 0;
            for (const ChunkCoord& c : lod.desiredChunks(center, "bedrock")) {
                if (bedrockMeshes.count(c)) continue;
                Chunk* chunk = bedrock->loadChunk(c, bedrockGen.fn, bedrockGen.user_data);
                if (!chunk) continue;
                bedrockMeshes.emplace(c, ChunkMesh::build(*chunk));
                if (++loaded >= kStreamPerFrame) break;
            }
            std::vector<ChunkCoord> toEvict;
            for (const auto& kv : bedrockMeshes)
                if (lod.shouldEvict(center, kv.first, "bedrock")) toEvict.push_back(kv.first);
            for (const ChunkCoord& c : toEvict) {
                bedrockMeshes[c].destroy();
                bedrockMeshes.erase(c);
                bedrock->unloadChunk(c);
            }
        }

        // ── Trigger decomposition for nearby macro voxels (approach) ──────────
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
                        const chunkmath::VoxelCoord V = chunkmath::chunkLocalToVoxel(kv.first, x, y, z, n);
                        if (!decomp.needsDecompose(V)) continue;
                        const WorldCoord ctr = chunkmath::voxelCenter(V, blocks->voxelSizeM());
                        if (glm::length(ctr.value - camPos.value) > kDecomposeRadiusM) continue;
                        if (!decomp.markPending(V)) continue;
                        worker.enqueue(makeJob(V));
                        ++enqueued;
                    }
        }

        // ── Integrate completed decomposition jobs ────────────────────────────
        std::unordered_set<ChunkCoord, ChunkCoordHash> compositeToRemesh;
        for (DecompositionResult& result : worker.drain()) {
            for (auto& chunkPtr : result.chunks) {
                const ChunkCoord tcc = chunkPtr->coord();
                Chunk* inserted = terrain->insertChunk(std::move(chunkPtr));
                if (!inserted) continue;
                auto it = terrainMeshes.find(tcc);
                if (it != terrainMeshes.end()) { it->second.destroy(); it->second = ChunkMesh::build(*inserted); }
                else                            { terrainMeshes.emplace(tcc, ChunkMesh::build(*inserted)); }
            }
            const chunkmath::LocalVoxel lv =
                chunkmath::voxelToChunkLocal(result.macro, blocks->chunkSizeVoxels());
            auto cit = blocks->chunks().find(lv.chunk);
            if (cit != blocks->chunks().end()) {
                if (!haveBlockTemplate) { blockTemplate = cit->second->at(lv.x, lv.y, lv.z); haveBlockTemplate = true; }
                cit->second->at(lv.x, lv.y, lv.z) = Voxel::empty();
                compositeToRemesh.insert(lv.chunk);
            }
            decomp.markDecomposed(result.macro);
        }

        // ── Release decomposed terrain past the keep radius (revert to block) ──
        std::vector<ChunkCoord> terrainToEvict;
        for (const auto& kv : terrainMeshes) {
            const chunkmath::VoxelCoord V{kv.first.x, kv.first.y, kv.first.z};
            const WorldCoord ctr = chunkmath::voxelCenter(V, blocks->voxelSizeM());
            if (glm::length(ctr.value - camPos.value) > kTerrainKeepRadiusM)
                terrainToEvict.push_back(kv.first);
        }
        for (const ChunkCoord& tcc : terrainToEvict) {
            auto it = terrainMeshes.find(tcc);
            if (it != terrainMeshes.end()) { it->second.destroy(); terrainMeshes.erase(it); }
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
            if (it != blocksMeshes.end()) { it->second.destroy(); it->second = ChunkMesh::build(*chunk); }
        }

        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        for (const auto& kv : bedrockMeshes) {
            const Chunk* chunk = bedrock->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), bedrock->voxelSizeM());
        }
        for (const auto& kv : blocksMeshes) {
            const Chunk* chunk = blocks->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), blocks->voxelSizeM());
        }
        for (const auto& kv : terrainMeshes) {
            const Chunk* chunk = terrain->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), terrain->voxelSizeM());
        }
        renderer.render();
    }

    std::cout << "[main] Decomposed macro voxels this session: " << decomp.decomposedCount() << "\n";
    for (auto& kv : blocksMeshes)  kv.second.destroy();
    for (auto& kv : terrainMeshes) kv.second.destroy();
    for (auto& kv : bedrockMeshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

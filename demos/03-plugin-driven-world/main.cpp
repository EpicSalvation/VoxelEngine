// M4 demo — plugin-driven world.
//
// The entire world is produced by plugins loaded from disk as real shared
// libraries (built under build/plugins/):
//
//   base-terrain  — registers the materials and the "terrain" layer generator;
//                   on its own the output is identical to the M3 demo.
//   water         — registers a "water" material and a feature generator that
//                   floods every empty voxel up to a fixed sea level. Removable.
//
// Press P to load/unload the water plugin at runtime. Resident chunks are
// dropped and re-streamed, so flat blue water appears or disappears in place —
// a direct, visible demonstration that removing a plugin removes its effect.
// With water unloaded the world is byte-for-byte the base-terrain heightmap.
//
// Controls: WASD move, Space/Shift up/down, mouse look, F toggles cursor,
// P toggles the water plugin, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VOXEL_BASE_PLUGIN_PATH
#  define VOXEL_BASE_PLUGIN_PATH ""
#endif
#ifndef VOXEL_WATER_PLUGIN_PATH
#  define VOXEL_WATER_PLUGIN_PATH ""
#endif

namespace {
constexpr char   kLogCat[] = "demo03";
constexpr int    kLoadsPerFrame = 2;     // budget generated/meshed chunks per frame
constexpr float  kMoveSpeed     = 24.0f;
constexpr float  kMouseSens     = 0.002f;
}  // namespace

int main() {
    // Layer config: one terminal layer with chunk-streaming parameters.
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

    // Plugins: load the base-terrain plugin from disk (the whole world comes
    // from it). The water plugin is loaded on demand via the P key.
    PluginManager pluginManager;
    if (std::string(VOXEL_BASE_PLUGIN_PATH).empty()) {
        Log::error(kLogCat, "Fatal: base plugin path not configured at build time.");
        return 1;
    }
    if (pluginManager.loadPlugin(VOXEL_BASE_PLUGIN_PATH) == kInvalidPluginId) {
        Log::error(kLogCat, (std::string("Fatal: could not load base-terrain plugin from ")
                             + VOXEL_BASE_PLUGIN_PATH).c_str());
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

    Engine engine;
    engine.start();

    platform::Window window(1024, 768, "VoxelEngine — M4 Plugin-Driven World");

    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));

    World world(terrain);
    LODManager lod(layerConfig);
    lod.setVerticalBand(0, 0);

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    // The water plugin's contribution is a feature generator: after the base
    // layer generator fills a chunk, every registered feature generator is
    // applied in turn. With the water plugin unloaded the registry is empty and
    // this is a no-op, so the chunk is pure base terrain.
    auto applyFeatureGenerators = [&](Chunk& chunk) {
        for (const auto& f : pluginManager.featureGenerators())
            if (f.fn)
                f.fn(chunk.origin(), world.voxelSizeM(), world.chunkSizeVoxels(),
                     chunk.data(), nullptr, 0, 0u, f.user_data);
    };

    // Drop every resident chunk so the world re-streams from scratch. Called
    // when the plugin set changes, since generated voxels copy their materials
    // by value — a chunk already in memory will not reflect the new plugin set
    // until it is regenerated.
    auto regenerateWorld = [&]() {
        for (auto& kv : meshes) {
            world.unloadChunk(kv.first);
            kv.second.destroy();
        }
        meshes.clear();
    };

    PluginId waterPluginId = kInvalidPluginId;
    const std::string waterPath = VOXEL_WATER_PLUGIN_PATH;

    // Camera: start above the terrain, free-look enabled.
    float      pitch = -0.5f, yaw = 0.0f;
    WorldCoord camPos(0.0, 45.0, 0.0);
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true;
    bool       cursorCaptured = true;
    bool       prevKeyF = false;
    bool       prevKeyP = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    Log::info(kLogCat, "Plugin-driven world. WASD + mouse to fly, Space/Shift up/down, "
                       "F toggles cursor, P toggles the water plugin, ESC quits. "
                       "Water plugin is OFF - press P to load it.");

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

        // P — load/unload the water plugin, then regenerate the resident world.
        bool curKeyP = (glfwGetKey(glfwWin, GLFW_KEY_P) == GLFW_PRESS);
        if (curKeyP && !prevKeyP) {
            if (waterPluginId == kInvalidPluginId) {
                if (waterPath.empty()) {
                    Log::warn(kLogCat, "Water plugin path not configured at build time.");
                } else {
                    waterPluginId = pluginManager.loadPlugin(waterPath);
                    if (waterPluginId == kInvalidPluginId)
                        Log::warn(kLogCat, (std::string("Failed to load water plugin from ")
                                            + waterPath).c_str());
                    else {
                        Log::info(kLogCat, "Water plugin ON.");
                        regenerateWorld();
                    }
                }
            } else {
                pluginManager.unloadPlugin(waterPluginId);
                waterPluginId = kInvalidPluginId;
                Log::info(kLogCat, "Water plugin OFF.");
                regenerateWorld();
            }
        }
        prevKeyP = curKeyP;

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

        // WASD + Space/Shift — camera-relative horizontal, world-up vertical.
        float sp = std::sin(pitch), cp = std::cos(pitch);
        float sy = std::sin(yaw),   cy = std::cos(yaw);
        glm::dvec3 fwd  {static_cast<double>(cp * sy), static_cast<double>(sp),
                         static_cast<double>(cp * cy)};
        glm::dvec3 right{static_cast<double>(cy), 0.0, static_cast<double>(-sy)};
        glm::dvec3 delta{0.0, 0.0, 0.0};
        double step = static_cast<double>(kMoveSpeed * dt);
        if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += fwd   * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= fwd   * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) delta -= right * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) delta += right * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) delta.y += step;
        if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) delta.y -= step;
        camPos = WorldCoord(camPos.value + delta);

        // ── Stream chunks around the camera ──────────────────────────────
        ChunkCoord center =
            chunkmath::worldToChunk(camPos, world.voxelSizeM(), world.chunkSizeVoxels());

        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain")) {
            if (meshes.count(c)) continue;
            Chunk* chunk = world.loadChunk(c, generator, generatorUserData);
            if (!chunk) continue;
            applyFeatureGenerators(*chunk);
            meshes.emplace(c, ChunkMesh::build(*chunk));
            if (++loaded >= kLoadsPerFrame) break;
        }

        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, "terrain"))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& c : toEvict) {
            meshes[c].destroy();
            meshes.erase(c);
            world.unloadChunk(c);
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

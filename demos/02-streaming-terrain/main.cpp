// M3 demo — streaming terrain flythrough.
//
// A single terminal layer of procedurally generated heightmap terrain whose
// chunks stream in and out around the free-flying camera within a view-distance
// budget. Chunks outside the budget (plus a hysteresis margin) are evicted, so
// the resident set stays bounded no matter how far you fly. Floating-origin
// submission keeps geometry precise arbitrarily far from the world origin.
//
// Controls: WASD move, Space/Shift up/down, mouse look, F toggles cursor capture,
// ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "plugins/ExamplePlugin.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace {
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
            std::cerr << "[main] Fatal: layer config error: " << e.what() << "\n";
            std::exit(1);
        }
    }();
    const LayerDef& terrain = layerConfig.layers().front();

    // Plugins: pull in the example heightmap generator.
    PluginManager pluginManager;
    pluginManager.wireInPlugin(voxel_plugin_init);

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

    Engine engine;
    engine.start();

    platform::Window window(1024, 768, "VoxelEngine — M3 Streaming Terrain");

    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));

    // Chunked world + LOD budget. The heightmap is vertically bounded, so a
    // single chunk-Y band holds all terrain (heights 6..28 < 32).
    World world(terrain);
    LODManager lod(layerConfig);
    lod.setVerticalBand(0, 0);

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    // Lifecycle hooks (none registered by the example plugin, but wired so a
    // plugin that registers them sees chunks stream).
    auto fireChunkCreated = [&](WorldCoord origin) {
        for (const auto& h : pluginManager.chunkCreatedHooks())
            if (h.layer_name == "terrain" && h.fn) h.fn(origin, h.user_data);
    };
    auto fireChunkEvicted = [&](WorldCoord origin) {
        for (const auto& h : pluginManager.chunkEvictedHooks())
            if (h.layer_name == "terrain" && h.fn) h.fn(origin, h.user_data);
    };

    // Camera: start above the terrain, free-look enabled.
    float      pitch = -0.5f, yaw = 0.0f;
    WorldCoord camPos(0.0, 45.0, 0.0);
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true;
    bool       cursorCaptured = true;
    bool       prevKeyF = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    std::cout << "[main] Streaming terrain. WASD + mouse to fly, Space/Shift up/down, "
                 "F toggles cursor, ESC quits.\n";

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

        // Load missing desired chunks, budgeted per frame.
        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain")) {
            if (meshes.count(c)) continue;
            Chunk* chunk = world.loadChunk(c, generator, generatorUserData);
            if (!chunk) continue;
            meshes.emplace(c, ChunkMesh::build(*chunk));
            fireChunkCreated(chunk->origin());
            if (++loaded >= kLoadsPerFrame) break;
        }

        // Evict chunks beyond the eviction radius (load radius + hysteresis).
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, "terrain"))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& c : toEvict) {
            WorldCoord origin =
                chunkmath::chunkOrigin(c, world.voxelSizeM(), world.chunkSizeVoxels());
            meshes[c].destroy();
            meshes.erase(c);
            world.unloadChunk(c);
            fireChunkEvicted(origin);
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

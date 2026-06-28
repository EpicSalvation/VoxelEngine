// ===========================================================================
// main.cpp — game entrypoint template
//
// A complete, runnable skeleton for a single-layer voxel game: it opens a
// window, loads a world-generation plugin from disk, streams chunks around a
// fly-camera, and lets the player break/place voxels with the mouse. Copy this
// file to demos/<NN-yourgame>/main.cpp (the build auto-discovers any
// demos/<dir>/main.cpp — no CMake edits needed) and start cutting it down or
// building it up to fit your game.
//
// What this template deliberately covers, because nearly every game needs it:
//   1. Engine + window + renderer bring-up.
//   2. Loading a world plugin and pulling its layer generator.
//   3. The chunk streaming loop (load near, evict far) — the heart of an
//      infinite world.
//   4. A fly-camera driven by WASD + mouse.
//   5. Raycast picking + break/place edits with a targeting highlight.
//
// What it leaves for you (cross-references to the tutorial series):
//   * Walk-mode + gravity + collision .......... tutorial 09 (kinematic-body)
//   * HUD / inventory / health ................. tutorial 09
//   * Audio ................................... tutorial 10
//   * Multiplayer ............................. tutorial 11
//   * Multi-layer worlds & decomposition ...... tutorial 07
//   * Performance tuning ...................... tutorial 14
//
// Every API used here is exercised by demos/03-plugin-driven-world (streaming)
// and demos/04-build-break-persist (raycast + edits); read those alongside this.
// ===========================================================================

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
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

// The build injects the absolute path of each plugin's compiled artifact as a
// preprocessor define (see CMakeLists.txt: VOXEL_<NAME>_PLUGIN_PATH). Replace
// this with the define for YOUR plugin. The fallback keeps the file compiling
// even before the define is wired up.
#ifndef VOXEL_BASE_PLUGIN_PATH
#  define VOXEL_BASE_PLUGIN_PATH ""
#endif

namespace {
constexpr char  kLogCat[]      = "game";   // log category tag for this game
constexpr int   kLoadsPerFrame = 4;        // chunk generate+mesh budget per frame
constexpr float kMoveSpeed     = 24.0f;    // fly-camera speed, m/s
constexpr float kMouseSens     = 0.002f;   // radians per pixel of mouse motion
constexpr double kReachM       = 8.0;      // how far the player can reach to edit
}  // namespace

int main() {
    // -----------------------------------------------------------------------
    // 1. World shape. Load the layer config. Here it is embedded inline for a
    //    self-contained template; in a real game prefer
    //    LayerConfig::loadFromFile("world.yaml") (see world.yaml in this dir).
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // 2. Plugins. Load your world-generation plugin from disk and pull out the
    //    layer generator it registered. The plugin contributes everything about
    //    the world's content (materials, terrain shape); the engine just streams
    //    and renders what the generator produces.
    // -----------------------------------------------------------------------
    PluginManager pluginManager;
    if (std::string(VOXEL_BASE_PLUGIN_PATH).empty()) {
        Log::error(kLogCat, "Fatal: world plugin path not configured at build time.");
        return 1;
    }
    if (pluginManager.loadPlugin(VOXEL_BASE_PLUGIN_PATH) == kInvalidPluginId) {
        Log::error(kLogCat, (std::string("Fatal: could not load world plugin from ")
                             + VOXEL_BASE_PLUGIN_PATH).c_str());
        return 1;
    }

    // Find the generator registered for our terminal layer ("terrain"). A plugin
    // may register several; match by layer name.
    LayerGeneratorFn generator         = nullptr;
    void*            generatorUserData = nullptr;
    for (const auto& g : pluginManager.layerGenerators()) {
        if (g.layer_name == terrain.name) {
            generator         = g.fn;
            generatorUserData = g.user_data;
        }
    }
    if (!generator) {
        Log::error(kLogCat, "Fatal: plugin registered no generator for the 'terrain' layer.");
        return 1;
    }

    // -----------------------------------------------------------------------
    // 3. Engine, window, renderer.
    // -----------------------------------------------------------------------
    Engine engine;
    engine.start();

    platform::Window window(1280, 720, "My Voxel Game");

    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));

    // The World holds resident chunks for the (single) terminal layer. The
    // LODManager decides which chunks should be resident around a center chunk.
    World      world(terrain);
    LODManager lod(layerConfig);
    lod.setVerticalBand(0, 0);  // stream only the y=0 chunk slab (a flat world).
                                // Widen this band for tall/voluminous worlds.

    // GPU meshes, one per resident chunk, keyed by chunk coordinate.
    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    // Rebuild the GPU mesh for the chunk owning a just-edited voxel. The mesher
    // emits opaque border faces with no cross-chunk culling, so an edit needs
    // only the owning chunk re-meshed (ARCHITECTURE §9).
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

    // Write one voxel, fire the plugins' on_voxel_modified hooks (so audio /
    // structural / networking plugins react), and re-mesh. No-op if the cell's
    // chunk is not resident. Pass Voxel::empty() to remove.
    auto editVoxel = [&](const chunkmath::VoxelCoord& vc, const Voxel& newVox) {
        const WorldCoord center = chunkmath::voxelCenter(vc, world.voxelSizeM());
        const Voxel oldVox = world.getVoxel(center);
        if (!world.setVoxel(center, newVox)) return;
        for (const auto& h : pluginManager.voxelModifiedHooks())
            if (h.fn) h.fn(center, &oldVox, &newVox, kLocalPlayer, h.user_data);
        remeshChunkOf(vc);
    };

    // -----------------------------------------------------------------------
    // 4. Camera + input state.
    // -----------------------------------------------------------------------
    float      pitch = -0.5f, yaw = 0.0f;       // look angles, radians
    WorldCoord camPos(0.0, 45.0, 0.0);          // eye position in world space
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true;
    bool       prevLeft = false, prevRight = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  // capture mouse

    auto prevTime = std::chrono::high_resolution_clock::now();
    Log::info(kLogCat, "WASD + mouse to fly, Space/Shift up/down, "
                       "Left-click break, Right-click place, ESC quits.");

    // -----------------------------------------------------------------------
    // 5. Main loop.
    // -----------------------------------------------------------------------
    while (!window.shouldClose()) {
        window.pollEvents();

        auto  now = std::chrono::high_resolution_clock::now();
        float dt  = std::chrono::duration<float>(now - prevTime).count();
        prevTime  = now;
        if (dt > 0.1f) dt = 0.1f;  // clamp huge hitches (e.g. after a breakpoint)

        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // ---- Mouse look --------------------------------------------------
        double mx, my;
        glfwGetCursorPos(glfwWin, &mx, &my);
        if (!firstMouse) {
            yaw   += static_cast<float>(mx - lastMouseX) * kMouseSens;
            pitch -= static_cast<float>(my - lastMouseY) * kMouseSens;
            if (pitch >  1.55f) pitch =  1.55f;   // clamp just shy of straight up
            if (pitch < -1.55f) pitch = -1.55f;   // and straight down (gimbal-safe)
        }
        lastMouseX = mx;
        lastMouseY = my;
        firstMouse = false;

        // ---- Movement: camera-relative horizontal, world-up vertical -----
        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);
        const glm::dvec3 fwd  {cp * sy, sp, cp * cy};   // forward look vector
        const glm::dvec3 right{cy, 0.0, -sy};           // strafe vector (level)
        glm::dvec3 delta{0.0, 0.0, 0.0};
        const double step = static_cast<double>(kMoveSpeed * dt);
        if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += fwd   * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= fwd   * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) delta -= right * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) delta += right * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) delta.y += step;
        if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) delta.y -= step;
        camPos = WorldCoord(camPos.value + delta);

        // ---- Stream chunks around the camera -----------------------------
        const ChunkCoord center =
            chunkmath::worldToChunk(camPos, world.voxelSizeM(), world.chunkSizeVoxels());

        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, terrain.name)) {
            if (meshes.count(c)) continue;                     // already resident
            Chunk* chunk = world.loadChunk(c, generator, generatorUserData);
            if (!chunk) continue;
            meshes.emplace(c, ChunkMesh::build(*chunk));
            if (++loaded >= kLoadsPerFrame) break;             // spread cost over frames
        }

        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, terrain.name))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& c : toEvict) {
            meshes[c].destroy();
            meshes.erase(c);
            world.unloadChunk(c);
        }

        // ---- Raycast picking + break/place -------------------------------
        const voxelcast::RayHit hit = voxelcast::raycast(world, camPos, fwd, kReachM);

        const bool left  = glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS;
        const bool right = glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

        if (hit.hit && left && !prevLeft) {
            editVoxel(hit.voxel, Voxel::empty());            // break the struck voxel
        } else if (hit.hit && right && !prevRight) {
            // Place into the empty cell the ray entered from. Copy the struck
            // voxel's material so the new block matches what you clicked on; pick
            // a material from an inventory in a real game.
            Voxel placed = world.getVoxel(chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()));
            editVoxel(hit.adjacent, placed);
        }
        prevLeft  = left;
        prevRight = right;

        // ---- Handle window resize ----------------------------------------
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        // ---- Render ------------------------------------------------------
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        for (const auto& kv : meshes) {
            const Chunk* chunk = world.getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin());
        }
        // Wireframe outline on the targeted voxel (yellow). Optional but it makes
        // editing feel responsive. The 4th arg is a 0..1 progress ramp — wire it
        // to mining progress for a hardness-gated break (see tutorial 09).
        if (hit.hit) {
            renderer.drawVoxelHighlight(
                chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()),
                static_cast<float>(world.voxelSizeM()), 0xff00ffff, -1.0f);
        }
        renderer.render();
    }

    // -----------------------------------------------------------------------
    // 6. Teardown. Destroy GPU resources before shutting the renderer down.
    // -----------------------------------------------------------------------
    for (auto& kv : meshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

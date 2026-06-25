// M15 demo — textured blocks.
//
// The acceptance artifact for M15 (Textured Voxels & Content-Tool Interop): the
// painting → export → import → render workflow the README's "Standard tool
// interoperability" bullet promises, shown end to end on a hand-authored
// Blockbench model.
//
// The bundled assets/blockbench/textured_blocks.bbmodel holds two blocks — a
// GRASS block (green grass_top, dirt bottom, grassy-edged dirt sides) and a
// multi-texture BRICK block (mossy top, plain stone bottom, running-bond brick
// sides) — each authored as a separate element with its own per-face textures,
// the textures embedded as base64 PNGs exactly as Blockbench exports a
// self-contained .bbmodel.
//
// What the demo does, in pipeline order (the four M15 seams in one place):
//   1. Loads the `blockbench` plugin, which registers a `.bbmodel` importer
//      (T6) on the register_importer seam.
//   2. Drives that importer over the sample file: it decodes each embedded
//      texture into the shared atlas (register_texture_data, T3), binds a
//      material per element with per-face tiles (set_material_faces, T4), and
//      fills a voxel grid — which the demo writes into a terminal layer. (The
//      engine's generic importer dispatch isn't bridged to the Layer API yet,
//      so the demo calls the registered ImporterFn directly, the same way the
//      BlockbenchImportTest does.)
//   3. TextureManager::rebuild() decodes the registered images, packs them into
//      one GPU atlas, and publishes each tile's UV sub-rect so the face
//      bindings resolve (T3).
//   4. ChunkMesh::build emits per-face atlas UVs for each voxel (T5); the voxel
//      fragment shader samples the atlas (T2) — so the grass block shows grass
//      on top and dirt on its sides, and the brick block its three faces.
//
// Press T to unbind the atlas (revert to the built-in 1x1 white tile): the
// blocks fall back to flat per-face color, making the texturing contribution
// visible. Press T again to rebind. This touches only the atlas binding — the
// baked-in UVs are unchanged — so it isolates exactly what the atlas adds.
//
// Controls: WASD + mouse-look fly, Space/Shift up/down, F toggles cursor,
//           T toggles the texture atlas, ESC quits.

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/TextureManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifndef VOXEL_BLOCKBENCH_PLUGIN_PATH
#  define VOXEL_BLOCKBENCH_PLUGIN_PATH ""
#endif
#ifndef VOXEL_REPO_ROOT
#  define VOXEL_REPO_ROOT "."
#endif

namespace {

constexpr float kFlySpeed  = 10.0f;
constexpr float kMouseSens = 0.002f;
constexpr int   kGrid      = 16;  // .bbmodel authoring cube maps to a 16^3 grid

// Read a whole file into bytes; returns false (and logs) when it can't be opened.
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return !out.empty();
}

// Find the .bbmodel importer the blockbench plugin registered.
const RegisteredImporter* bbImporter(const PluginManager& pm) {
    for (const auto& imp : pm.importers())
        if (imp.extension == "bbmodel" && imp.fn) return &imp;
    return nullptr;
}

}  // namespace

int main() {
    // ── Locate the sample asset (committed under the repo's assets/) ───────────
    const std::string bbmodelPath =
        std::string(VOXEL_REPO_ROOT) + "/assets/blockbench/textured_blocks.bbmodel";

    std::vector<uint8_t> bbBytes;
    if (!readFile(bbmodelPath, bbBytes)) {
        std::cerr << "[main] Fatal: cannot read sample model at " << bbmodelPath << "\n";
        return 1;
    }

    // ── Plugins: load the Blockbench importer ──────────────────────────────────
    PluginManager pluginManager;
    if (std::string(VOXEL_BLOCKBENCH_PLUGIN_PATH).empty() ||
        pluginManager.loadPlugin(VOXEL_BLOCKBENCH_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load blockbench plugin from '"
                  << VOXEL_BLOCKBENCH_PLUGIN_PATH << "'.\n";
        return 1;
    }
    const RegisteredImporter* importer = bbImporter(pluginManager);
    if (!importer) {
        std::cerr << "[main] Fatal: blockbench plugin registered no .bbmodel importer.\n";
        return 1;
    }

    // ── A single terminal layer to hold the imported blocks ────────────────────
    LayerConfig layerConfig = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: editor
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 32
    view_distance_chunks: 4
)");
        } catch (const std::exception& e) {
            std::cerr << "[main] Fatal: layer config error: " << e.what() << "\n";
            std::exit(1);
        }
    }();

    World  world(layerConfig);
    Layer* editor = world.layer("editor");
    if (!editor) {
        std::cerr << "[main] Fatal: editor layer missing.\n";
        return 1;
    }
    editor->loadChunk({0, 0, 0}, nullptr);

    // ── Window + renderer ──────────────────────────────────────────────────────
    platform::Window window(1024, 768, "VoxelEngine — M15 Textured Blocks");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));

    // TextureManager owns the GPU atlas; it must be constructed after bgfx is up
    // (renderer.initialize) and self-registers with the PluginManager so the
    // importer's register_texture_data calls feed it.
    texture::TextureManager textureManager(pluginManager, renderer);

    // ── Run the importer: fill a grid, then write it into the layer ────────────
    std::vector<Voxel> grid(static_cast<size_t>(kGrid) * kGrid * kGrid, Voxel::empty());
    const int rc = importer->fn(bbBytes.data(), bbBytes.size(),
                                WorldCoord(0, 0, 0), kGrid, grid.data(),
                                importer->user_data);
    if (rc != 0) {
        std::cerr << "[main] Fatal: .bbmodel import failed (code " << rc << ").\n";
        return 1;
    }

    auto idx3 = [](int x, int y, int z) {
        return static_cast<size_t>(x) +
               static_cast<size_t>(kGrid) *
                   (static_cast<size_t>(y) + static_cast<size_t>(kGrid) * z);
    };
    int placed = 0;
    for (int z = 0; z < kGrid; ++z)
    for (int y = 0; y < kGrid; ++y)
    for (int x = 0; x < kGrid; ++x) {
        const Voxel& v = grid[idx3(x, y, z)];
        if (v.isEmpty()) continue;
        editor->setVoxel(WorldCoord(x + 0.5, y + 0.5, z + 0.5), v);
        ++placed;
    }
    std::cout << "[main] Imported " << bbmodelPath << " — " << placed
              << " voxels placed." << std::endl;

    // Build the atlas now that the importer has registered every texture: decode,
    // pack, upload, and publish the per-tile UV sub-rects the face bindings (T4)
    // resolve against. After this the mesh builder can emit textured UVs.
    std::cout << "[main] Registered " << pluginManager.textures().size()
              << " textures from the model." << std::endl;

    textureManager.rebuild();
    std::cout << "[main] Atlas built — " << textureManager.tileCount() << " tiles."
              << std::endl;

    // ── Mesh the resident chunk(s) ─────────────────────────────────────────────
    std::vector<std::pair<ChunkCoord, ChunkMesh>> meshes;
    for (const auto& [coord, chunk] : editor->chunks())
        meshes.emplace_back(coord, ChunkMesh::build(*chunk, editor->voxelSizeM()));

    // ── Camera ─────────────────────────────────────────────────────────────────
    float pitch = -0.35f, yaw = 0.0f;
    WorldCoord camPos(8.0, 9.0, -10.0);
    double lastMX = 0.0, lastMY = 0.0;
    bool firstMouse = true, cursorCaptured = true;
    bool prevKeyF = false, prevKeyT = false;
    bool atlasOn = true;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    renderer.setHudText({
        "M15 Textured Blocks  —  Blockbench import end-to-end",
        "left: grass block (grass top / dirt sides)   right: brick block (mossy top / brick sides)",
        "WASD + mouse fly | Space/Shift up/down | F cursor | T toggle atlas | ESC quit",
        "atlas: ON",
    });

    auto prevTime = std::chrono::high_resolution_clock::now();

    // ── Main loop ──────────────────────────────────────────────────────────────
    while (!window.shouldClose()) {
        window.pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // Toggle cursor capture (F).
        bool curF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curF && !prevKeyF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                             cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevKeyF = curF;

        // Toggle the atlas binding (T): textured ↔ flat per-face color. Only the
        // bound texture changes; the meshes' baked UVs are untouched.
        bool curT = (glfwGetKey(glfwWin, GLFW_KEY_T) == GLFW_PRESS);
        if (curT && !prevKeyT) {
            atlasOn = !atlasOn;
            const bgfx::TextureHandle invalid = BGFX_INVALID_HANDLE;
            renderer.setAtlas(atlasOn ? textureManager.atlas() : invalid);
            renderer.setHudText({
                "M15 Textured Blocks  —  Blockbench import end-to-end",
                "left: grass block (grass top / dirt sides)   right: brick block (mossy top / brick sides)",
                "WASD + mouse fly | Space/Shift up/down | F cursor | T toggle atlas | ESC quit",
                atlasOn ? "atlas: ON" : "atlas: OFF (flat per-face color)",
            });
        }
        prevKeyT = curT;

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
            lastMX = mx; lastMY = my;
            firstMouse = false;
        }

        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);
        glm::dvec3 fwd  {cp * sy, sp,  cp * cy};
        glm::dvec3 right{cy,      0.0, -sy    };
        glm::dvec3 delta{0.0, 0.0, 0.0};
        double step = static_cast<double>(kFlySpeed * dt);
        if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += fwd   * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= fwd   * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) delta -= right * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) delta += right * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) delta.y += step;
        if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) delta.y -= step;
        camPos = WorldCoord(camPos.value + delta);

        // ── Render ─────────────────────────────────────────────────────────────
        {
            int curFbW, curFbH;
            window.framebufferSize(curFbW, curFbH);
            renderer.setViewport(curFbW, curFbH);
        }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        for (const auto& [coord, mesh] : meshes) {
            WorldCoord origin = chunkmath::chunkOrigin(coord, editor->voxelSizeM(),
                                                       editor->chunkSizeVoxels());
            renderer.renderChunk(mesh, origin, editor->voxelSizeM(), editor->chunkSizeVoxels());
        }

        renderer.render();
    }

    for (auto& [coord, mesh] : meshes) mesh.destroy();
    meshes.clear();
    renderer.shutdown();
    return 0;
}

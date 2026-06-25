// M7 demo — MagicaVoxel round-trip.
//
// Demonstrates the Engine-level .vox import/export path introduced in M7:
//
//   - On startup the demo imports a small MagicaVoxel .vox file
//     (demos/06-magicavoxel-round-trip/assets/test_model.vox) into an "editor"
//     terminal layer at world origin via Engine::importVox.
//   - If the asset file does not yet exist the demo generates a 4×4×4 coloured
//     cube using VoxExporter and saves it as the asset, then imports it.
//   - The editor layer is rendered through the existing mesh pipeline at 1 m
//     voxel scale; the same free-camera and place/remove controls from M5
//     are available (left/right mouse to break/place, 1–9 to pick material).
//   - Press E to export the current "editor" layer back to a new .vox file
//     (output.vox beside the working directory). The terminal logs whether
//     auto-chunking triggered (region > 256 voxels per axis) and whether the
//     lossy-property warning fired (extended material fields present but not
//     representable in the .vox format).
//
// Controls: WASD move, mouse look, Space/Shift up/down (fly),
// left/right mouse break/place, 1–9 material, E export, F cursor, ESC quit.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "io/VoxExporter.h"
#include "io/VoxImporter.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <climits>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

#ifndef DEMO_ASSET_DIR
#  define DEMO_ASSET_DIR ""
#endif

namespace {

constexpr float  kFlySpeed  = 16.0f;
constexpr float  kMouseSens = 0.002f;
constexpr double kReachM    = 12.0;

// Palette entries available for building.
struct SimpleMat { const char* name; uint8_t idx; };
static const SimpleMat kMaterials[] = {
    {"palette-1", 1}, {"palette-2", 2}, {"palette-3", 3}, {"palette-4", 4},
    {"palette-5", 5}, {"palette-6", 6}, {"palette-7", 7}, {"palette-8", 8},
    {"palette-9", 9},
};

// Generate a 4×4×4 coloured cube as the test asset if it does not yet exist.
bool generateTestAsset(const std::string& path) {
    LayerDef def;
    def.name              = "editor";
    def.voxel_size_m      = 1.0;
    def.mode              = VoxelMode::terminal;
    def.chunk_size_voxels = 16;
    def.view_distance_chunks = 1;

    World world(def);
    Layer* layer = world.layer("editor");
    if (!layer) return false;

    layer->loadChunk({0, 0, 0}, nullptr);

    // Four quadrants of palette colour (indices 1–4) so each face looks distinct.
    for (int z = 0; z < 4; ++z)
    for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
        Voxel v;
        v.material.palette_index = static_cast<uint8_t>(1 + (x / 2) + 2 * (z / 2));
        layer->setVoxel(WorldCoord(x + 0.5, y + 0.5, z + 0.5), v);
    }

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    VoxExporter exporter;
    return exporter.save(path, *layer, WorldCoord(0, 0, 0), WorldCoord(4, 4, 4));
}

}  // namespace

int main() {
    // ── Asset path ────────────────────────────────────────────────────────────
    const std::string assetDir =
        std::string(DEMO_ASSET_DIR)[0] != '\0' ? std::string(DEMO_ASSET_DIR) : ".";
    const std::string assetVox  = assetDir + "/test_model.vox";
    const std::string outputVox = "output.vox";

    if (!std::filesystem::exists(assetVox)) {
        std::cout << "[main] test_model.vox not found; generating test asset at "
                  << assetVox << "\n";
        if (!generateTestAsset(assetVox)) {
            std::cerr << "[main] Fatal: could not generate test asset.\n";
            return 1;
        }
        std::cout << "[main] Asset generated.\n";
    }

    // ── Layer / world ─────────────────────────────────────────────────────────
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

    PluginManager pluginManager;
    World world(layerConfig);

    // ── Engine I/O dispatch ───────────────────────────────────────────────────
    Engine engine;
    engine.init(pluginManager, world);

    std::cout << "[main] Importing " << assetVox << " ...\n";
    if (!engine.importVox(assetVox, "editor", WorldCoord(0, 0, 0))) {
        std::cerr << "[main] Fatal: .vox import failed.\n";
        return 1;
    }
    std::cout << "[main] Import complete.\n";

    // ── Window + renderer ─────────────────────────────────────────────────────
    platform::Window window(1024, 768, "VoxelEngine — M7 MagicaVoxel Round-Trip");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    Layer* editorLayer = world.layer("editor");
    if (!editorLayer) {
        std::cerr << "[main] Fatal: editor layer missing.\n";
        return 1;
    }

    // ── Mesh cache ────────────────────────────────────────────────────────────
    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    auto buildMeshFor = [&](const Chunk& chunk) {
        auto it = meshes.find(chunk.coord());
        if (it != meshes.end()) { it->second.destroy(); meshes.erase(it); }
        meshes.emplace(chunk.coord(), ChunkMesh::build(chunk));
    };

    // Mesh every chunk already resident after import.
    for (const auto& [coord, chunk] : editorLayer->chunks())
        buildMeshFor(*chunk);

    auto remeshChunkOf = [&](const chunkmath::VoxelCoord& vc) {
        ChunkCoord cc =
            chunkmath::voxelToChunkLocal(vc, editorLayer->chunkSizeVoxels()).chunk;
        const Chunk* chunk = editorLayer->getChunk(cc);
        if (!chunk) return;
        buildMeshFor(*chunk);
    };

    // ── Camera state ──────────────────────────────────────────────────────────
    float pitch = -0.3f, yaw = 0.0f;
    WorldCoord camPos(8.0, 10.0, -8.0);
    double lastMX = 0.0, lastMY = 0.0;
    bool firstMouse = true, cursorCaptured = true;
    bool prevKeyF = false, prevKeyE = false;
    bool prevLeft = false, prevRight = false;
    size_t selectedMat = 0;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    std::cout << "[main] WASD + mouse = fly  |  left/right mouse = break/place\n"
                 "[main] 1-9 = material  |  E = export to output.vox  |  F = cursor  |  ESC = quit\n";

    // ── Main loop ─────────────────────────────────────────────────────────────
    while (!window.shouldClose()) {
        window.pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // Toggle cursor.
        bool curF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curF && !prevKeyF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                             cursorCaptured ? GLFW_CURSOR_DISABLED
                                            : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevKeyF = curF;

        // Export on E.
        bool curE = (glfwGetKey(glfwWin, GLFW_KEY_E) == GLFW_PRESS);
        if (curE && !prevKeyE) {
            std::cout << "[main] Exporting editor layer to " << outputVox << " ...\n";

            bool lossyWarned = false;
            bool autoChunked = false;

            Log::setWarnHandler([&](const char* msg) {
                std::string s(msg);
                std::cerr << "[WARN] " << s << "\n";
                if (s.find("extended voxel properties dropped") != std::string::npos)
                    lossyWarned = true;
            });

            // Compute bounding box from resident chunks.
            WorldCoord expMin(0, 0, 0), expMax(32, 32, 32);
            if (!editorLayer->chunks().empty()) {
                int minX = INT_MAX, minY = INT_MAX, minZ = INT_MAX;
                int maxX = INT_MIN, maxY = INT_MIN, maxZ = INT_MIN;
                for (const auto& [cc, _] : editorLayer->chunks()) {
                    if (cc.x < minX) minX = cc.x;
                    if (cc.y < minY) minY = cc.y;
                    if (cc.z < minZ) minZ = cc.z;
                    if (cc.x > maxX) maxX = cc.x;
                    if (cc.y > maxY) maxY = cc.y;
                    if (cc.z > maxZ) maxZ = cc.z;
                }
                int cs = editorLayer->chunkSizeVoxels();
                double vsz = editorLayer->voxelSizeM();
                expMin = WorldCoord(minX * cs * vsz, minY * cs * vsz, minZ * cs * vsz);
                expMax = WorldCoord((maxX + 1) * cs * vsz,
                                   (maxY + 1) * cs * vsz,
                                   (maxZ + 1) * cs * vsz);
                glm::dvec3 extent = expMax.value - expMin.value;
                if (extent.x > 256 * vsz || extent.y > 256 * vsz || extent.z > 256 * vsz)
                    autoChunked = true;
            }

            bool ok = engine.exportVox("editor", expMin, expMax, outputVox);
            Log::setWarnHandler(nullptr);

            if (ok) {
                std::cout << "[main] Export complete -> " << outputVox << "\n";
                std::cout << "[main] Auto-chunking: "
                          << (autoChunked ? "YES - region exceeded 256 voxels per axis."
                                          : "no - region fits in a single 256^3 object.")
                          << "\n";
                if (lossyWarned)
                    std::cout << "[main] Lossy-property warning: extended properties dropped.\n";
            } else {
                std::cerr << "[main] Export failed.\n";
            }
        }
        prevKeyE = curE;

        // Material selection.
        for (int i = 0; i < 9; ++i) {
            if (glfwGetKey(glfwWin, GLFW_KEY_1 + i) == GLFW_PRESS &&
                selectedMat != static_cast<size_t>(i)) {
                selectedMat = static_cast<size_t>(i);
                std::cout << "[main] Selected: " << kMaterials[selectedMat].name << "\n";
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

        // Raycast for place/remove (uses World, not just editorLayer).
        glm::dvec3 lookDir{cp * sy, sp, cp * cy};
        auto rayHit = voxelcast::raycast(world, camPos, lookDir, kReachM);

        bool mouseL = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS);
        bool mouseR = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        if (rayHit.hit) {
            if (mouseL && !prevLeft) {
                const WorldCoord center =
                    chunkmath::voxelCenter(rayHit.voxel, editorLayer->voxelSizeM());
                editorLayer->setVoxel(center, Voxel::empty());
                remeshChunkOf(rayHit.voxel);
            }
            if (mouseR && !prevRight) {
                const WorldCoord placeCenter =
                    chunkmath::voxelCenter(rayHit.adjacent, editorLayer->voxelSizeM());
                ChunkCoord placeCC =
                    chunkmath::voxelToChunkLocal(
                        rayHit.adjacent, editorLayer->chunkSizeVoxels()).chunk;
                editorLayer->loadChunk(placeCC, nullptr);
                Voxel v;
                v.material.palette_index = kMaterials[selectedMat].idx;
                editorLayer->setVoxel(placeCenter, v);
                remeshChunkOf(rayHit.adjacent);
            }
        }
        prevLeft  = mouseL;
        prevRight = mouseR;

        // ── Render ────────────────────────────────────────────────────────────
        {
            int curFbW, curFbH;
            window.framebufferSize(curFbW, curFbH);
            renderer.setViewport(curFbW, curFbH);
        }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        for (const auto& [coord, mesh] : meshes) {
            WorldCoord origin =
                chunkmath::chunkOrigin(coord, editorLayer->voxelSizeM(),
                                      editorLayer->chunkSizeVoxels());
            renderer.renderChunk(mesh, origin, editorLayer->voxelSizeM(), editorLayer->chunkSizeVoxels());
        }

        if (rayHit.hit) {
            WorldCoord hCenter =
                chunkmath::voxelCenter(rayHit.voxel, editorLayer->voxelSizeM());
            renderer.drawVoxelHighlight(hCenter,
                                        static_cast<float>(editorLayer->voxelSizeM()));
        }

        renderer.render();
    }

    for (auto& [coord, mesh] : meshes) mesh.destroy();
    meshes.clear();
    renderer.shutdown();

    return 0;
}

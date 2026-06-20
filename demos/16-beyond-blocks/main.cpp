// M16 demo — Beyond blocks.
//
// A deliberately NON-Minecraft configuration on the same engine. There is no
// "down": gravity is the zero-g policy (M16 L7), so the camera flies freely in
// six degrees of freedom through empty space. The only interactive content is a
// single finite floating island — a domed top tapering to a pointed underside,
// empty above, below, AND on every side — a shape no heightmap can produce. Far
// out in every direction hangs a vast, sparse immutable backdrop shell the player
// never reaches.
//
// What it proves about the generalized engine (M16):
//   * Axis-agnostic streaming (L1). The island streams as a camera-centered BOX
//     volume with no vertical bias — fly above or below it and it stays resident,
//     where the pre-M16 absolute-Y band would have emptied. The backdrop streams
//     as a thin SHELL volume: resident only at range, never the solid sky between.
//   * Heterogeneous per-layer budgets (L5). A tiny tight playspace box and a vast
//     sparse backdrop shell stream simultaneously, each capped by its OWN
//     resident_chunk_budget — radically different scales and densities in one
//     stack. The HUD reads both resident counts side by side.
//   * No gravity axis (L7). The world's gravity policy is GravityProvider::zeroG()
//     — gravityAt() returns the zero vector everywhere, so movement has no
//     vertical bias and there is no canonical "up". The same seam a body-bound
//     demo (Asteroid belt miner) reads for a radial well reads "no down" here.
//
// Content comes from the floating-playspace plugin (M16 C1): a "playspace"
// volumetric generator (the island) and a "backdrop" generator (the shell). Both
// are pure functions of world position + a fixed seed, so streamed-out chunks
// regenerate byte-identically (ARCHITECTURE §4).
//
// This is a flythrough: there is no voxel editing. Fly around the island and out
// toward the backdrop to watch the two volumes stream under their own budgets.
//
// Controls: WASD move, mouse look, Space/Shift up/down, F cursor, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "world/ChunkCoordMath.h"
#include "world/GravityProvider.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VOXEL_FLOATING_PLAYSPACE_PLUGIN_PATH
#  define VOXEL_FLOATING_PLAYSPACE_PLUGIN_PATH ""
#endif

namespace {

constexpr float  kFlySpeed  = 60.0f;
constexpr float  kMouseSens = 0.002f;
constexpr int    kLoadsPerFrame = 16;  // chunks generated+meshed per layer per frame

constexpr uint64_t kWorldSeed = 0xB3402DB10C5ull;  // matches the plugin's seed comment

// Backdrop solid geometry sits at ~480–560 m world radius; the island spans
// ~±48 m horizontally and ~24–71 m vertically around (0, 64, 0). A camera kept
// near the island never approaches the shell, so a 1200 m far clip comfortably
// frames both with room to spare for the floating-origin depth budget.
constexpr float  kFarClipM = 1200.0f;
constexpr double kVFovDeg  = 60.0;

// Conservative view-frustum cull for chunk bounding spheres, built from the same
// camera basis and projection the renderer uses (identical to demo 10). After a
// long flight the mesh stores hold many chunks behind the camera or outside the
// view cone; this skips submitting them without ever culling visible geometry.
struct Frustum {
    glm::dvec3 pos{}, fwd{}, right{}, up{};
    double tanH = 0.0, tanV = 0.0, cosH = 1.0, cosV = 1.0, farClip = 0.0;

    void update(const WorldCoord& camPos, float pitch, float yaw,
                double aspect, double vfovDeg, double farClipM) {
        pos = camPos.value;
        const double cp = std::cos(pitch), sp = std::sin(pitch);
        const double cy = std::cos(yaw),   sy = std::sin(yaw);
        fwd   = glm::dvec3(cp * sy, sp, cp * cy);
        right = glm::dvec3(cy, 0.0, -sy);
        up    = glm::cross(fwd, right);
        tanV  = std::tan(glm::radians(vfovDeg) * 0.5);
        tanH  = tanV * aspect;
        cosV  = 1.0 / std::sqrt(1.0 + tanV * tanV);
        cosH  = 1.0 / std::sqrt(1.0 + tanH * tanH);
        farClip = farClipM;
    }

    bool sphereVisible(const glm::dvec3& center, double radius) const {
        const glm::dvec3 rel = center - pos;
        const double z = glm::dot(rel, fwd);
        if (z + radius < 0.0 || z - radius > farClip) return false;
        const double x = glm::dot(rel, right);
        if ((std::abs(x) - z * tanH) * cosH > radius) return false;
        const double y = glm::dot(rel, up);
        if ((std::abs(y) - z * tanV) * cosV > radius) return false;
        return true;
    }
};

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// A layer the demo streams directly through the M16 StreamingVolume (via
// LODManager), one of the two heterogeneous residency cases. Terminal layers
// have no engine-owned streamer (the DecompositionManager streams only composite
// roots and decomposition children); a standalone terminal/immutable layer is
// streamed by the front-end exactly as demo 02 does, now over an axis-agnostic
// volume rather than an XZ-disc × Y-band.
struct StreamedLayer {
    Layer*           layer = nullptr;
    LayerGeneratorFn genFn = nullptr;
    // The generator reads the layer's voxel size through user_data (a pointer to a
    // stable double); the engine does not inject it. 1 m for the island, 8 m for
    // the backdrop — so the SAME field function fills coarse distant blocks and
    // fine island voxels at the correct world scale (the plugin's contract).
    double           voxelSizeM = 1.0;
    MeshStore        meshes;
};

int chebyshev(const ChunkCoord& a, const ChunkCoord& b) {
    return std::max({std::abs(a.x - b.x), std::abs(a.y - b.y), std::abs(a.z - b.z)});
}

// Stream one layer for a frame: load the chunks its StreamingVolume wants (up to
// a per-frame cap, building meshes for the non-empty ones), evict chunks that
// have left the volume, then enforce the layer's own resident_chunk_budget
// (farthest-first). The budget bounds memory even though the box volume admits
// many empty-air chunks around a finite island.
void streamLayer(StreamedLayer& s, const LODManager& lod, const WorldCoord& camPos,
                 int budget) {
    Layer& layer = *s.layer;
    const ChunkCoord camChunk =
        chunkmath::worldToChunk(camPos, layer.voxelSizeM(), layer.chunkSizeVoxels());

    // Load (capped per frame so the initial fill spreads across frames).
    int loaded = 0;
    for (const ChunkCoord& cc : lod.desiredChunks(camChunk, layer.name())) {
        if (loaded >= kLoadsPerFrame) break;
        if (layer.getChunk(cc)) continue;
        if (Chunk* chunk = layer.loadChunk(cc, s.genFn, &s.voxelSizeM)) {
            ChunkMesh mesh = ChunkMesh::build(*chunk);
            if (!mesh.empty()) s.meshes.emplace(cc, std::move(mesh));  // skip all-air
            ++loaded;
        }
    }

    // Evict chunks beyond the volume + hysteresis margin.
    std::vector<ChunkCoord> stale;
    for (const auto& kv : layer.chunks())
        if (lod.shouldEvict(camChunk, kv.first, layer.name()))
            stale.push_back(kv.first);
    for (const ChunkCoord& cc : stale) {
        if (auto it = s.meshes.find(cc); it != s.meshes.end()) {
            it->second.destroy();
            s.meshes.erase(it);
        }
        layer.unloadChunk(cc);
    }

    // Enforce the per-layer resident-chunk budget (M16 L5 heterogeneous budgets):
    // shed the farthest-first chunks until the resident set fits. Immutable and
    // recipe-free chunks regenerate deterministically on re-entry, so this is
    // free to drop them.
    if (budget > 0 && static_cast<int>(layer.chunks().size()) > budget) {
        std::vector<ChunkCoord> resident;
        resident.reserve(layer.chunks().size());
        for (const auto& kv : layer.chunks()) resident.push_back(kv.first);
        std::sort(resident.begin(), resident.end(),
                  [&](const ChunkCoord& a, const ChunkCoord& b) {
                      return chebyshev(a, camChunk) > chebyshev(b, camChunk);
                  });
        for (const ChunkCoord& cc : resident) {
            if (static_cast<int>(layer.chunks().size()) <= budget) break;
            if (auto it = s.meshes.find(cc); it != s.meshes.end()) {
                it->second.destroy();
                s.meshes.erase(it);
            }
            layer.unloadChunk(cc);
        }
    }
}

void renderLayer(const StreamedLayer& s, BgfxRenderer& renderer, const Frustum& frustum) {
    const Layer& layer = *s.layer;
    const double chunkWorld   = layer.voxelSizeM() * layer.chunkSizeVoxels();
    const double sphereRadius = chunkWorld * 0.8660254;  // √3/2 · side, the half-diagonal
    for (const auto& kv : s.meshes) {
        const Chunk* chunk = layer.getChunk(kv.first);
        if (!chunk || kv.second.empty()) continue;
        const glm::dvec3 center = chunk->origin().value + glm::dvec3(chunkWorld * 0.5);
        if (!frustum.sphereVisible(center, sphereRadius)) continue;
        renderer.renderChunk(kv.second, chunk->origin(), layer.voxelSizeM());
    }
}

}  // namespace

int main() {
    // ── Layer config ──────────────────────────────────────────────────────────
    // Two layers, coarsest-first (the validation rule), with NO decomposition
    // chain — neither layer is composite:
    //   backdrop  — immutable, 8 m voxels, a SHELL volume. The distant surround.
    //   playspace — terminal + interactive (M16 L4), 1 m voxels, a BOX volume.
    // The 8:1 size ratio satisfies the adjacent-layer integer-ratio rule. The
    // shell band (view_distance 5 − thickness 2 = inner 3 → [3,5] chunks of 128 m
    // = [384, 640] m) brackets the plugin's 480–560 m backdrop; the box's 8-chunk
    // radius of 16 m chunks (128 m) covers the ±48 m island from any side.
    LayerConfig cfg = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: backdrop
    voxel_size_m: 8.0
    mode: immutable
    chunk_size_voxels: 16
    view_distance_chunks: 5
    resident_chunk_budget: 256
    streaming_volume:
      shape: shell
      shell_thickness_chunks: 2

  - name: playspace
    voxel_size_m: 1.0
    mode: terminal
    interactive: true
    chunk_size_voxels: 16
    view_distance_chunks: 8
    resident_chunk_budget: 512
    streaming_volume:
      shape: box
)");
        } catch (const std::exception& e) {
            std::cerr << "[main] Fatal: " << e.what() << "\n";
            std::exit(1);
        }
    }();

    // ── Plugin loading ──────────────────────────────────────────────────────────
    if (std::string(VOXEL_FLOATING_PLAYSPACE_PLUGIN_PATH).empty()) {
        std::cerr << "[main] Fatal: floating-playspace plugin not configured at build time.\n";
        return 1;
    }

    World world(cfg);
    Engine engine;
    PluginManager pm;
    engine.init(pm, world);

    if (pm.loadPlugin(VOXEL_FLOATING_PLAYSPACE_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load floating-playspace plugin from "
                  << VOXEL_FLOATING_PLAYSPACE_PLUGIN_PATH << "\n";
        return 1;
    }

    // ── Resolve the two streamed layers and their generators ─────────────────────
    auto setUpLayer = [&](const char* name) -> StreamedLayer {
        StreamedLayer s;
        s.layer = world.layer(name);
        if (!s.layer) {
            std::cerr << "[main] Fatal: layer '" << name << "' not found.\n";
            std::exit(1);
        }
        s.voxelSizeM = s.layer->voxelSizeM();
        for (const auto& g : pm.layerGenerators())
            if (g.layer_name == name) { s.genFn = g.fn; break; }
        if (!s.genFn) {
            std::cerr << "[main] Fatal: no generator registered for layer '" << name << "'.\n";
            std::exit(1);
        }
        return s;
    };
    StreamedLayer backdrop  = setUpLayer("backdrop");
    StreamedLayer playspace = setUpLayer("playspace");

    const int backdropBudget  = cfg.findLayer("backdrop")->resident_chunk_budget;
    const int playspaceBudget = cfg.findLayer("playspace")->resident_chunk_budget;

    // ── Gravity policy: zero-g (M16 L7) ──────────────────────────────────────────
    // The whole point of "Beyond blocks": there is no "down". gravityAt() returns
    // the zero vector everywhere, so the kinematic step has no vertical bias and a
    // collision step would report no grounded state. Flight is pure 6-DOF.
    const GravityProvider gravity = GravityProvider::zeroG();

    // ── Renderer ──────────────────────────────────────────────────────────────
    platform::Window window(1280, 720, "VoxelEngine — M16 Beyond Blocks");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setFarClip(kFarClipM);
    renderer.setCrosshair(true);

    // ── Camera (free flight) ────────────────────────────────────────────────────
    // Start off to one side of the island, looking back at it.
    float      pitch = -0.15f, yaw = 3.14159f;  // facing -Z toward the island
    WorldCoord camPos(0.0, 70.0, 150.0);
    double     lastMX = 0, lastMY = 0;
    bool       firstMouse = true, cursorCaptured = true, prevF = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    std::cout << "[main] Controls: WASD fly, Space/Shift up/down, F cursor, ESC quit.\n"
              << "[main] Zero-g flythrough: the island streams as a BOX, the backdrop as a SHELL.\n";

    auto prevTime = std::chrono::high_resolution_clock::now();

    // ── Main loop ─────────────────────────────────────────────────────────────
    while (!window.shouldClose()) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;  // clamp streaming hitches

        window.pollEvents();
        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // ── Mouse look ──────────────────────────────────────────────────────────
        if (cursorCaptured) {
            double mx, my;
            glfwGetCursorPos(glfwWin, &mx, &my);
            if (firstMouse) { lastMX = mx; lastMY = my; firstMouse = false; }
            yaw   += static_cast<float>((mx - lastMX) * kMouseSens);
            pitch -= static_cast<float>((my - lastMY) * kMouseSens);
            pitch  = std::max(-1.5f, std::min(1.5f, pitch));
            lastMX = mx; lastMY = my;
        }

        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw),   sy = std::sin(yaw);
        const glm::dvec3 look(cp * sy, sp, cp * cy);
        const glm::dvec3 right(cy, 0, -sy);

        // ── F: toggle cursor capture ────────────────────────────────────────────
        const bool curF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curF && !prevF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevF = curF;

        // ── Free-flight movement (zero-g: no vertical bias) ─────────────────────
        glm::dvec3 wish(0);
        if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) wish += look;
        if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) wish -= look;
        if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) wish -= glm::dvec3(right);
        if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) wish += glm::dvec3(right);
        if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) wish.y += 1.0;
        if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) wish.y -= 1.0;
        if (glm::length(wish) > 0.0) wish = glm::normalize(wish);
        camPos = WorldCoord(camPos.value + wish * (kFlySpeed * static_cast<double>(dt)));

        // ── Stream both layers under their own StreamingVolume + budget ──────────
        LODManager lod(cfg);
        streamLayer(backdrop,  lod, camPos, backdropBudget);
        streamLayer(playspace, lod, camPos, playspaceBudget);

        // ── HUD ─────────────────────────────────────────────────────────────────
        {
            const glm::dvec3 down = gravity.gravityAt(camPos);  // zero-g ⇒ (0,0,0)
            const bool hasDown = glm::length(down) > 1e-9;
            char hud[256];
            std::snprintf(hud, sizeof(hud),
                "gravity=%s | playspace(box) chunks=%d/%d | backdrop(shell) chunks=%d/%d",
                hasDown ? "down" : "zero-g (no down)",
                static_cast<int>(playspace.layer->chunks().size()), playspaceBudget,
                static_cast<int>(backdrop.layer->chunks().size()),  backdropBudget);
            renderer.setHudText({std::string(hud)});
        }

        // ── Render ────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        Frustum frustum;
        frustum.update(camPos, pitch, yaw,
                       static_cast<double>(fbW) / static_cast<double>(fbH),
                       kVFovDeg, kFarClipM);

        // Coarsest first so finer voxels occlude coarser ones via the depth test.
        renderLayer(backdrop,  renderer, frustum);
        renderLayer(playspace, renderer, frustum);

        renderer.render();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    for (auto* s : {&backdrop, &playspace})
        for (auto& [cc, mesh] : s->meshes)
            mesh.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

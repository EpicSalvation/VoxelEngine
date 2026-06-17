// M14 demo — flow and heat.
//
// The payoff for M14's fluid/thermal field simulation (docs/ARCHITECTURE.md
// §17): two engine-owned sparse overlays — a fluid-amount field and a
// temperature field — advance every frame at terminal-voxel granularity, and
// the engine NEVER writes a voxel for either. As with M13's structural
// collapse, the engine only DETECTS and reports; a removable response plugin
// owns every world write.
//
// Both fields are diffusion-like and gated by a material property, so the demo
// presents them as a matched pair, each drawn as a colored field tint sampled
// through the engine's read-only accessors:
//   • FLUID is gated by `porosity`. A glass-fronted tank holds an impermeable
//     rock shell (porosity 0) split by a POROUS sand dam (porosity 0.08). A
//     fluid emitter fills the 3-deep left chamber; the field pools against the
//     impermeable walls (sharp edge) and SEEPS slowly through the dam (a blue
//     gradient) into the right chamber, which visibly lags. Where the field
//     saturates, the mandatory `flow` plugin realizes a translucent water voxel.
//   • HEAT is gated by `thermal_conductivity`. The tank floor is a high-
//     conductivity IRON half and a low-conductivity ROCK half; a heat emitter on
//     the iron races warmth across it (a wide orange plume) while the rock half
//     barely warms — the exact analog of porosity gating fluid.
//
// `field-sources` (always loaded) registers both emitters. `flow` (togglable)
// is the MANDATORY fluid responder — separate plugins so the detect/respond
// split is visible at runtime.
//
// What the demo makes visible:
//   • porosity- vs conductivity-gated diffusion, side by side, as field tints.
//   • the fluid SEEP — the blue field advances through the dam ahead of the
//     realized water, so the seep is visible before any voxel appears.
//   • the budgeted passes — a HUD line reads the live active fluid/thermal cell
//     counts, the per-frame event counts, and the carried fluid backlog.
//   • the detect/respond split — press 0 to UNLOAD `flow`: the fields keep
//     simulating (fed by the still-loaded emitters) but no fluid geometry is
//     realized — the legitimate "fluid never becomes voxels" configuration.
//     Press 1 to reload it. Heat is always field-only — it never becomes voxels.
//
// Controls: WASD + mouse-look fly, Space/Shift up/down, F toggles cursor,
//           1 = load flow (fluid realizes), 0 = unload flow (field-only),
//           ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/Tuning.h"
#include "net/NetworkManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/Palette.h"
#include "simulation/FluidSystem.h"
#include "simulation/ThermalSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifndef VOXEL_FLOW_PLUGIN_PATH
#  define VOXEL_FLOW_PLUGIN_PATH ""
#endif
#ifndef VOXEL_FIELD_SOURCES_PLUGIN_PATH
#  define VOXEL_FIELD_SOURCES_PLUGIN_PATH ""
#endif

using chunkmath::VoxelCoord;

namespace {

constexpr float  kFlySpeed  = 12.0f;
constexpr float  kMouseSens = 0.002f;
constexpr double kReachM    = 40.0;

// Single terminal layer covering [0,32) m in one chunk — the field systems work
// at terminal-voxel granularity, so no composite/decomposition stack is needed.
const char* kLayerYaml = R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 32
)";

constexpr uint8_t kWaterPalette = 5;   // engine's translucent water slot
constexpr uint8_t kGlassPalette = 8;   // repurposed for a translucent glass wall

// ── Scene geometry (terminal voxel coordinates, 1 m each) ───────────────────
// A glass-fronted tank, 3 voxels deep (z=15..17) so the chambers hold a real
// volume that fills gradually: floor at y=1, rim walls y=2..9, a 2-thick porous
// dam at x=12..13 splitting the interior into a left (x=9..11) and right
// (x=14..17) chamber. Viewed through the translucent front wall as an X–Y slab.
constexpr int64_t kZ0     = 15, kZ1 = 17;       // interior z span (3 deep)
constexpr int64_t kZBack  = 14, kZFront = 18;   // back (opaque) / front (glass) walls
constexpr int64_t kX0     = 8,  kX1 = 18;       // tank x span (walls at the ends)
constexpr int64_t kFloorY = 1;                  // impermeable floor row
constexpr int64_t kRimY0  = 2,  kRimY1 = 9;     // rim wall height span
constexpr int64_t kSeamX  = 12;                 // iron (<=) / rock (>) floor seam
constexpr int64_t kDamX0  = 12, kDamX1 = 13;    // 2-thick porous dam columns
constexpr int64_t kDamY0  = 2,  kDamY1 = 7;     // porous dam height span

constexpr float kAmbient = tuning::thermal::kAmbientTemperature;  // 20 °C

// rock: impermeable, poorly conducting. The rim walls and the resistive floor
// half. porosity 0 blocks fluid entirely; low conductivity barely carries heat.
Voxel rock() {
    Voxel v;
    v.material.density              = 2600.0f;
    v.material.thermal_conductivity = 0.08f;
    v.material.porosity             = 0.0f;
    v.material.hardness             = 0.6f;
    v.material.palette_index        = 1;    // gray
    return v;
}

// iron: impermeable, highly conducting. The conductive floor half — heat races
// across it, so the heat plume is visibly asymmetric about the seam.
Voxel iron() {
    Voxel v;
    v.material.density              = 7800.0f;
    v.material.thermal_conductivity = 0.9f;
    v.material.porosity             = 0.0f;
    v.material.hardness             = 0.8f;
    v.material.palette_index        = 11;   // metallic blue-gray
    return v;
}

// sand: the porous dam. Low porosity (0.08), two cells thick, so fluid seeps
// through slowly and the dam itself never saturates into water — it stays a
// barrier, and the right chamber visibly lags the left as it fills.
Voxel sand() {
    Voxel v;
    v.material.density              = 1500.0f;
    v.material.thermal_conductivity = 0.3f;
    v.material.porosity             = 0.08f;
    v.material.hardness             = 0.3f;
    v.material.palette_index        = 4;    // tan
    return v;
}

// glass: impermeable (porosity 0) but translucent, for the front wall the camera
// looks through. Same physical role as rock; only the palette differs.
Voxel glass() {
    Voxel v = rock();
    v.material.palette_index = kGlassPalette;
    return v;
}

// Ramp a temperature to an ABGR tint: ambient→hot maps orange→white-hot.
uint32_t heatColor(float t) {
    const float k = std::clamp((t - kAmbient) / 160.0f, 0.0f, 1.0f);
    const auto r = static_cast<uint32_t>(255);
    const auto g = static_cast<uint32_t>(110.0f + 145.0f * k);
    const auto b = static_cast<uint32_t>(40.0f * k + 200.0f * k * k);
    return 0xff000000u | (b << 16) | (g << 8) | r;   // ABGR: 0xAABBGGRR
}

// Ramp a fluid amount [0,1+] to an ABGR tint: faint blue → bright cyan.
uint32_t fluidColor(float a) {
    const float k = std::clamp(a, 0.0f, 1.0f);
    const auto b = static_cast<uint32_t>(150.0f + 105.0f * k);
    const auto g = static_cast<uint32_t>(60.0f + 150.0f * k);
    return 0xff000000u | (b << 16) | (g << 8) | 0u;  // R=0
}

}  // namespace

int main() {
    // ── World + single terminal layer ───────────────────────────────────────
    LayerConfig cfg = [] {
        try {
            return LayerConfig::loadFromString(kLayerYaml);
        } catch (const std::exception& e) {
            std::cerr << "[main] Fatal: layer config error: " << e.what() << "\n";
            std::exit(1);
        }
    }();

    World world(cfg);
    Layer* terrain = world.layer("terrain");
    if (!terrain) {
        std::cerr << "[main] Fatal: expected a 'terrain' layer.\n";
        return 1;
    }
    const double cvs = terrain->voxelSizeM();
    const ChunkCoord kChunk{0, 0, 0};
    terrain->loadChunk(kChunk, nullptr);

    // A translucent glass color for the front wall (ABGR, alpha < 0xff routes it
    // through the renderer's alpha-blended pass). Must be set before meshing.
    palette::setColor(kGlassPalette, 0x50f0e8d8u);

    // Build the static scene directly (bypassing the edit path, so construction
    // fires no field events). Fluid voxels are NOT built here — they are
    // realized at runtime by the flow plugin.
    auto set = [&](int64_t x, int64_t y, int64_t z, const Voxel& v) {
        terrain->setVoxel(chunkmath::voxelCenter({x, y, z}, cvs), v);
    };
    // Floor across the tank footprint: iron up to the seam, rock past it.
    for (int64_t z = kZBack; z <= kZFront; ++z)
        for (int64_t x = kX0; x <= kX1; ++x)
            set(x, kFloorY, z, x <= kSeamX ? iron() : rock());
    // Rim walls: opaque rock on the ends and back; translucent glass on the front
    // (z = kZFront, nearest the camera) so the interior is visible.
    for (int64_t y = kRimY0; y <= kRimY1; ++y) {
        for (int64_t z = kZBack; z <= kZFront; ++z) { set(kX0, y, z, rock()); set(kX1, y, z, rock()); }
        for (int64_t x = kX0; x <= kX1; ++x) {
            set(x, y, kZBack,  rock());
            set(x, y, kZFront, glass());
        }
    }
    // Porous dam across the interior z slices (NOT touching the z-walls, or it
    // would punch a permeable hole through them).
    for (int64_t z = kZ0; z <= kZ1; ++z)
        for (int64_t y = kDamY0; y <= kDamY1; ++y)
            for (int64_t x = kDamX0; x <= kDamX1; ++x)
                set(x, y, z, sand());

    // ── Engine / edit plumbing (mirrors the M13 demo) ───────────────────────
    PluginManager pm;
    Engine engine;
    engine.init(pm, world);

    net::NetworkManager nm;
    nm.init(world, pm);

    // ── Field systems ────────────────────────────────────────────────────────
    // ThermalSystem is built once and persists; FluidSystem is rebuilt whenever
    // the flow plugin is toggled, so its rising/falling bookkeeping starts clean
    // for the new responder configuration (the M13 physics.reset() pattern).
    auto thermal = std::make_unique<sim::ThermalSystem>(world, pm);
    auto fluid   = std::make_unique<sim::FluidSystem>(world, pm);
    engine.setThermalSystem(thermal.get());
    engine.setFluidSystem(fluid.get());

    // ── Edit tracking → remesh (host-owned on_voxel_modified hook) ───────────
    struct EditTracker {
        std::unordered_set<ChunkCoord, ChunkCoordHash> touched;
        int    chunkSize = 32;
        double voxelSize = 1.0;
    } tracker;
    tracker.chunkSize = terrain->chunkSizeVoxels();
    tracker.voxelSize = cvs;
    pm.registerEngineVoxelModifiedHook(
        [](WorldCoord pos, const Voxel*, const Voxel*, PlayerId, void* ud) {
            auto* t = static_cast<EditTracker*>(ud);
            const VoxelCoord v = chunkmath::worldToVoxel(pos, t->voxelSize);
            t->touched.insert(chunkmath::voxelToChunkLocal(v, t->chunkSize).chunk);
        },
        &tracker);

    // The emitters (fluid + heat). Loaded once and never unloaded, so the fields
    // keep simulating even when the flow responder is dropped.
    if (std::strlen(VOXEL_FIELD_SOURCES_PLUGIN_PATH) == 0 ||
        pm.loadPlugin(VOXEL_FIELD_SOURCES_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load field-sources plugin from '"
                  << VOXEL_FIELD_SOURCES_PLUGIN_PATH << "'.\n";
        return 1;
    }

    // Clear every realized water voxel in the chunk (host edit → empty).
    auto clearWaterVoxels = [&]() {
        const Chunk* ch = terrain->getChunk(kChunk);
        if (!ch) return;
        const int n = terrain->chunkSizeVoxels();
        for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const Voxel& v = ch->at(x, y, z);
                    if (!v.isEmpty() && v.material.palette_index == kWaterPalette) {
                        const VoxelCoord vc = chunkmath::chunkLocalToVoxel(kChunk, x, y, z, n);
                        nm.applyEdit(kLocalPlayer, chunkmath::voxelCenter(vc, cvs),
                                     Voxel::empty());
                    }
                }
    };

    // ── flow responder load/unload (1 load / 0 unload) ───────────────────────
    PluginId flowPlugin = kInvalidPluginId;
    bool     flowLoaded = false;
    auto setFlow = [&](bool load) {
        if (flowPlugin != kInvalidPluginId) {
            pm.unloadPlugin(flowPlugin);
            flowPlugin = kInvalidPluginId;
        }
        // Reset the fluid scene: drop realized geometry and rebuild the system so
        // its announced-rising set is clean for the new responder configuration.
        clearWaterVoxels();
        fluid.reset();
        fluid = std::make_unique<sim::FluidSystem>(world, pm);
        engine.setFluidSystem(fluid.get());

        if (load) {
            if (std::strlen(VOXEL_FLOW_PLUGIN_PATH) == 0 ||
                (flowPlugin = pm.loadPlugin(VOXEL_FLOW_PLUGIN_PATH)) == kInvalidPluginId) {
                std::cerr << "[main] Warning: could not load flow plugin from '"
                          << VOXEL_FLOW_PLUGIN_PATH << "' — staying field-only.\n";
                load = false;
            }
        }
        flowLoaded = load;
        std::cout << "[main] Fluid response: "
                  << (flowLoaded ? "flow (realizes water voxels)"
                                 : "NONE (field-only, no geometry)") << "\n";
    };
    setFlow(true);   // start with the responder loaded

    // ── Renderer ─────────────────────────────────────────────────────────────
    platform::Window window(1280, 720, "VoxelEngine — M14 Flow and Heat");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;
    auto rebuildChunk = [&](const ChunkCoord& cc) {
        auto it = meshes.find(cc);
        if (it != meshes.end()) { it->second.destroy(); meshes.erase(it); }
        const Chunk* ch = terrain->getChunk(cc);
        if (!ch) return;
        ChunkMesh m = ChunkMesh::build(*ch);
        if (!m.empty()) meshes.emplace(cc, std::move(m));
    };
    rebuildChunk(kChunk);

    // ── Camera (fly) — in front of the glass, looking slightly down into the tank
    float      pitch = -0.12f, yaw = 3.14159265f;   // yaw=π → forward points -Z
    WorldCoord camPos(13.0, 7.0, 30.0);
    double     lastMX = 0, lastMY = 0;
    bool       firstMouse = true, cursorCaptured = true;
    bool       prevF = false, prev0 = false, prev1 = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    std::cout <<
        "[main] Flow and heat — water fills the left chamber and SEEPS through the\n"
        "[main] porous sand dam into the right; heat races across the iron floor and\n"
        "[main] barely warms the rock. Blue tint = fluid field, orange = heat field.\n"
        "[main] Keys: 1 load flow (realizes water), 0 unload (field-only), F cursor,\n"
        "[main] ESC quit.\n";

    while (!window.shouldClose()) {
        window.pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // F: toggle cursor capture.
        const bool curF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curF && !prevF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                             cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevF = curF;

        // 0/1: toggle the fluid responder live (no engine change).
        const bool cur0 = (glfwGetKey(glfwWin, GLFW_KEY_0) == GLFW_PRESS);
        const bool cur1 = (glfwGetKey(glfwWin, GLFW_KEY_1) == GLFW_PRESS);
        if (cur0 && !prev0 && flowLoaded)  setFlow(false);
        if (cur1 && !prev1 && !flowLoaded) setFlow(true);
        prev0 = cur0; prev1 = cur1;

        // Mouse look.
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
        const glm::dvec3 look(static_cast<double>(cp * sy), static_cast<double>(sp),
                              static_cast<double>(cp * cy));
        const glm::dvec3 right(static_cast<double>(cy), 0.0, static_cast<double>(-sy));

        glm::dvec3 wish(0.0);
        if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) wish += look;
        if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) wish -= look;
        if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) wish -= right;
        if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) wish += right;
        if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) wish.y += 1.0;
        if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) wish.y -= 1.0;
        if (glm::length(wish) > 0.0)
            camPos = WorldCoord(camPos.value +
                                glm::normalize(wish) * (kFlySpeed * static_cast<double>(dt)));

        // ── End-of-frame field passes (after this frame's edits applied) ─────
        // thermal first, then fluid: fluid's events drive flow's apply_edit,
        // whose voxel writes land through the same choke point and re-dirty for
        // the next frame's pass (the M13 feedback loop, reused for fluid).
        thermal->tick(dt);
        fluid->tick(dt);

        // Remesh exactly the chunks any edit (the flow plugin's) touched.
        for (const ChunkCoord& cc : tracker.touched) rebuildChunk(cc);
        tracker.touched.clear();

        // ── Render ───────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        for (const auto& kv : meshes) {
            const Chunk* ch = terrain->getChunk(kv.first);
            if (ch) renderer.renderChunk(kv.second, ch->origin(), cvs);
        }

        // ── Field tints (the two read-only accessors), over the interior slab ─
        // Heat lives in the conductive solids (floor); fluid lives in the open
        // chambers — tint solids by temperature, air cells by fluid amount, so
        // the two never fight over a cell. Drawn front-to-back across the slab so
        // the seep front is caught in whichever z row it has reached.
        for (int64_t z = kZ1; z >= kZ0; --z)
            for (int64_t y = kFloorY; y <= kRimY1; ++y)
                for (int64_t x = kX0; x <= kX1; ++x) {
                    const WorldCoord c = chunkmath::voxelCenter({x, y, z}, cvs);
                    const Voxel v = world.getVoxel(c);
                    if (v.isEmpty()) {
                        const float a = engine.fluidAmountAt(c);
                        if (a > 0.04f)
                            renderer.drawVoxelHighlight(c, static_cast<float>(cvs), fluidColor(a), -1.0f);
                    } else if (v.material.thermal_conductivity > 0.0f) {
                        const float t = engine.temperatureAt(c);
                        if (t > kAmbient + 1.0f)
                            renderer.drawVoxelHighlight(c, static_cast<float>(cvs), heatColor(t), -1.0f);
                    }
                }

        // ── Cursor probe + targeting highlight ───────────────────────────────
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, look, kReachM);
        float probeT = kAmbient, probeF = 0.0f;
        if (hit.hit) {
            const WorldCoord c = chunkmath::voxelCenter(hit.voxel, cvs);
            probeT = engine.temperatureAt(c);
            probeF = engine.fluidAmountAt(c);
            renderer.drawVoxelHighlight(c, static_cast<float>(cvs), 0xff00ffff, -1.0f);
        }

        // ── HUD ──────────────────────────────────────────────────────────────
        char line0[160];
        std::snprintf(line0, sizeof(line0),
                      "Fluid response: %s   [1 load flow | 0 unload (field-only)]",
                      flowLoaded ? "flow" : "NONE");
        char line1[200];
        std::snprintf(line1, sizeof(line1),
                      "Fluid cells: %zu  events: %d  backlog: %zu   |   "
                      "Thermal cells: %zu  events: %d",
                      fluid->activeCount(), fluid->eventsFiredLastTick(),
                      fluid->carryBacklog(),
                      thermal->activeCount(), thermal->eventsFiredLastTick());
        char line2[160];
        if (hit.hit)
            std::snprintf(line2, sizeof(line2),
                          "Probe [%lld,%lld,%lld]  temp: %.1f C  fluid: %.2f",
                          static_cast<long long>(hit.voxel.x),
                          static_cast<long long>(hit.voxel.y),
                          static_cast<long long>(hit.voxel.z), probeT, probeF);
        else
            std::snprintf(line2, sizeof(line2), "Probe: (aim at a voxel)");
        renderer.setHudText({std::string(line0), std::string(line1), std::string(line2)});

        renderer.render();
    }

    // Teardown: drop the field systems (each unregisters its engine hook) and the
    // host remesh tracker before the plugin manager so no torn-down callback is
    // left dangling.
    fluid.reset();
    thermal.reset();
    pm.unregisterEngineVoxelModifiedHook(&tracker);
    for (auto& kv : meshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

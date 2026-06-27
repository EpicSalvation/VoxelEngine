// M13 demo — structural collapse.
//
// The payoff for M13's upward damage propagation (docs/ARCHITECTURE.md §7): mine
// a composite structure until its aggregated structural_strength can no longer
// bridge the unsupported span, and watch it CAVE IN — with every world write owned
// by a removable plugin, never the engine.
//
// The scene is a stone BRIDGE: an 8-macro stone deck spanning between two
// immutable BEDROCK towers that rise from a bedrock floor. As built the deck is
// stable — every macro is within stone's support reach (~4.5 macros, span formula
// in tuning::physics) of one of the two bedrock anchors. Mine a deck macro near a
// tower and the freed left/right run can no longer reach an anchor, so the engine's
// PropagationSystem fires on_structural_event for each newly-unstable macro and the
// loaded response plugin clears (crumble) or relocates (falling-debris) it. Those
// edits return through the same on_voxel_modified choke point a player edit takes,
// re-dirtying the parent macro, so the NEXT end-of-frame pass finds the next ring —
// the cascade marches macro by macro and STOPS DEAD at the part still reachable
// from a bedrock tower (the immutable boundary).
//
// What the demo makes visible:
//   • the detect/respond split — the engine (PhysicsSystem) only detects + fires;
//     the structural-response PLUGIN owns every voxel write. Press 0 to unload it
//     and mining leaves the bridge cave-in-free (the Minecraft-style config).
//   • the same engine event drives different game feel with ZERO engine change —
//     press 1 for `crumble` (the deck vanishes) or 2 for `falling-debris` (the
//     deck tumbles down and piles on the bedrock floor).
//   • the budgeted feedback loop — a HUD line reads the per-frame structural-event
//     count and the carried-overflow backlog (events past kMaxStructuralEventsPerFrame
//     carry to the next frame instead of stalling one).
//
// Controls: WASD + mouse-look fly, Space/Shift up/down, F toggles cursor,
//           LEFT MOUSE mines the targeted stone voxel,
//           1 = crumble response, 2 = falling-debris response, 0 = no plugin,
//           R rebuilds the bridge, ESC quits.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "net/NetworkManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "simulation/PhysicsSystem.h"
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
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifndef VOXEL_CRUMBLE_PLUGIN_PATH
#  define VOXEL_CRUMBLE_PLUGIN_PATH ""
#endif
#ifndef VOXEL_FALLING_DEBRIS_PLUGIN_PATH
#  define VOXEL_FALLING_DEBRIS_PLUGIN_PATH ""
#endif

using chunkmath::VoxelCoord;

namespace {

constexpr char kLogCat[] = "demo13";

constexpr float  kFlySpeed  = 14.0f;
constexpr float  kMouseSens = 0.002f;
constexpr double kReachM    = 22.0;   // generous pick reach so the bridge is minable from a distance

// ── World layout (docs/ARCHITECTURE.md §7) ─────────────────────────────────────
// blocks (2 m composite → terrain) is the M13 detection level; terrain (1 m) is
// the editable terminal layer; bedrock (0.5 m immutable) is the anchor the
// collapse stops at. Single chunk per layer covers the whole [0,32) m scene.
const char* kLayerYaml = R"(
layers:
  - name: blocks
    voxel_size_m: 2.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 16
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 32
  - name: bedrock
    voxel_size_m: 0.5
    mode: immutable
    chunk_size_voxels: 64
)";

// Structure geometry in blocks-macro coordinates (1 unit = one 2 m macro voxel).
// A single macro-thick (z fixed) ribbon, so the whole scene reads face-on as a
// clean 2D side view: two bedrock pillars + floor framing a gray stone lintel.
constexpr int64_t kRatio   = 2;     // terrain voxels per macro edge (2 m / 1 m)
constexpr int64_t kZ0      = 8,  kZ1 = 8;   // 1 macro thick in z
constexpr int64_t kYFloor  = 2;             // bedrock floor macro-y
constexpr int64_t kYDeck   = 6;             // stone deck / tower-top macro-y
constexpr int64_t kXLTower = 3,  kXRTower = 12;   // the two bedrock anchor towers
constexpr int64_t kDeckX0  = 4,  kDeckX1  = 11;   // 8-macro stone deck between them

// stone: the deck material. structural_strength 0.9 → bridges ~4.5 macros, so an
// 8-macro deck anchored at BOTH ends is stable, but a run cut off from one anchor
// past ~4 macros caves. Matches the material-showcase "stone" palette (gray).
Voxel stone() {
    Voxel v;
    v.material.density             = 2700.0f;
    v.material.structural_strength = 0.9f;
    v.material.hardness            = 0.7f;
    v.material.palette_index       = 1;   // gray
    return v;
}

// bedrock: the immutable anchor. Very high strength and a negative (indestructible)
// hardness; lives in its own immutable layer so picking/edits never touch it.
Voxel bedrock() {
    Voxel v;
    v.material.density             = 5000.0f;
    v.material.structural_strength = 9.9f;
    v.material.hardness            = -1.0f;  // indestructible sentinel
    v.material.palette_index       = 10;     // near-black
    return v;
}

const char* responseName(int mode) {
    switch (mode) {
        case 1:  return "crumble";
        case 2:  return "falling-debris";
        default: return "NONE (cave-in-free)";
    }
}

}  // namespace

int main() {
    // ── World + layers ─────────────────────────────────────────────────────────
    LayerConfig cfg = [] {
        try {
            return LayerConfig::loadFromString(kLayerYaml);
        } catch (const std::exception& e) {
            Log::error(kLogCat, (std::string("Fatal: layer config error: ") + e.what()).c_str());
            std::exit(1);
        }
    }();

    World world(cfg);
    Layer* blocks  = world.layer("blocks");
    Layer* terrain = world.layer("terrain");
    Layer* bedrock_l = world.layer("bedrock");
    if (!blocks || !terrain || !bedrock_l) {
        Log::error(kLogCat, "Fatal: expected blocks/terrain/bedrock layers.");
        return 1;
    }

    const double mvs = blocks->voxelSizeM();      // macro (composite) voxel size, 2 m
    const double cvs = terrain->voxelSizeM();     // child (terminal) voxel size, 1 m
    const double bvs = bedrock_l->voxelSizeM();   // bedrock voxel size, 0.5 m

    // Resident chunks. The composite "blocks" chunk is loaded EMPTY so every macro
    // reads as decomposed (resident, no block voxel) — its aggregate then comes
    // from the terrain children, which is what mining changes (§7). One chunk of
    // each layer spans the whole scene.
    blocks->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    terrain->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    bedrock_l->loadChunk(ChunkCoord{0, 0, 0}, nullptr);

    // Fill one terminal (child) voxel — used to build the stone deck directly,
    // bypassing the edit path so construction fires no structural events.
    auto setChild = [&](VoxelCoord childV, const Voxel& v) {
        terrain->setVoxel(chunkmath::voxelCenter(childV, cvs), v);
    };
    // Fill all ratio³ terminal children of a macro with the given voxel.
    auto fillMacroStone = [&](VoxelCoord macro, const Voxel& v) {
        const VoxelCoord cmin = chunkmath::childVoxelMin(macro, kRatio);
        for (int64_t dz = 0; dz < kRatio; ++dz)
            for (int64_t dy = 0; dy < kRatio; ++dy)
                for (int64_t dx = 0; dx < kRatio; ++dx)
                    setChild({cmin.x + dx, cmin.y + dy, cmin.z + dz}, v);
    };
    // Fill a macro's whole volume with bedrock voxels (mvs/bvs per edge), so the
    // immutable layer is solid across the macro and isAnchor() samples it at the
    // macro center (PropagationSystem::isAnchor).
    auto fillMacroBedrock = [&](VoxelCoord macro) {
        const int64_t r = chunkmath::layerRatio(mvs, bvs);  // bedrock voxels per macro edge
        const VoxelCoord bmin{macro.x * r, macro.y * r, macro.z * r};
        for (int64_t dz = 0; dz < r; ++dz)
            for (int64_t dy = 0; dy < r; ++dy)
                for (int64_t dx = 0; dx < r; ++dx)
                    bedrock_l->setVoxel(
                        chunkmath::voxelCenter({bmin.x + dx, bmin.y + dy, bmin.z + dz}, bvs),
                        bedrock());
    };

    // Build the immutable bedrock: a floor slab plus the two anchor towers. Done
    // once — bedrock never changes (it is the boundary the cascade stops at).
    for (int64_t z = kZ0; z <= kZ1; ++z) {
        for (int64_t x = kXLTower; x <= kXRTower; ++x) fillMacroBedrock({x, kYFloor, z});  // floor
        for (int64_t y = kYFloor; y <= kYDeck; ++y) {
            fillMacroBedrock({kXLTower, y, z});  // left tower
            fillMacroBedrock({kXRTower, y, z});  // right tower
        }
    }

    // (Re)build the stone deck between the tower tops. Also used by the R reset.
    auto buildDeck = [&]() {
        for (int64_t z = kZ0; z <= kZ1; ++z)
            for (int64_t x = kDeckX0; x <= kDeckX1; ++x)
                fillMacroStone({x, kYDeck, z}, stone());
    };
    // Clear any stone/debris in the deck region (the macros a collapse can touch:
    // the deck row and the gap below it down to the floor), for the R reset.
    auto clearDebrisRegion = [&]() {
        for (int64_t z = kZ0; z <= kZ1; ++z)
            for (int64_t y = kYFloor + 1; y <= kYDeck; ++y)
                for (int64_t x = kDeckX0; x <= kDeckX1; ++x)
                    fillMacroStone({x, y, z}, Voxel::empty());
    };
    buildDeck();

    // ── Plugin / edit plumbing ─────────────────────────────────────────────────
    PluginManager pm;
    Engine engine;
    engine.init(pm, world);

    // The edit choke point: routes both player edits and the response plugin's
    // ctx.apply_edit through World::setVoxel + on_voxel_modified (Offline = applied
    // locally). PhysicsSystem rides that hook to observe every edit (§7, §15).
    net::NetworkManager nm;
    nm.init(world, pm);

    // The driver. Constructed after init so its engine-owned on_voxel_modified hook
    // is in place for every subsequent edit. unique_ptr so R can reset its cascade
    // bookkeeping (firedUnstable_/carry_/aggregates_) by rebuilding it.
    auto physics = std::make_unique<sim::PhysicsSystem>(world, pm);

    // ── Edit tracking → remesh ─────────────────────────────────────────────────
    // A host-owned on_voxel_modified hook records which terrain chunks any edit
    // touched (player OR plugin), so we remesh exactly those once per frame.
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

    // ── Response plugin selection (1 crumble / 2 falling-debris / 0 none) ───────
    int      responseMode   = 0;
    PluginId responsePlugin = kInvalidPluginId;
    auto setResponse = [&](int mode) {
        if (responsePlugin != kInvalidPluginId) {
            pm.unloadPlugin(responsePlugin);
            responsePlugin = kInvalidPluginId;
        }
        const char* path = (mode == 1) ? VOXEL_CRUMBLE_PLUGIN_PATH
                         : (mode == 2) ? VOXEL_FALLING_DEBRIS_PLUGIN_PATH
                                       : "";
        if (mode != 0) {
            if (std::string(path).empty() ||
                (responsePlugin = pm.loadPlugin(path)) == kInvalidPluginId) {
                Log::warn(kLogCat, (std::string("Could not load ") + responseName(mode)
                          + " plugin from '" + path + "' — staying cave-in-free.").c_str());
                mode = 0;
            }
        }
        responseMode = mode;
        Log::info(kLogCat, (std::string("Structural response: ") + responseName(responseMode)).c_str());
    };
    setResponse(1);  // start with crumble loaded

    // ── Renderer ───────────────────────────────────────────────────────────────
    platform::Window window(1280, 720, "VoxelEngine — M13 Structural Collapse");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    // Bedrock never changes → build its mesh once. Terrain meshes are rebuilt on
    // edit from the tracker. (The blocks layer is empty, so it has no mesh.)
    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> terrainMeshes;
    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> bedrockMeshes;
    auto rebuildTerrainChunk = [&](const ChunkCoord& cc) {
        auto it = terrainMeshes.find(cc);
        if (it != terrainMeshes.end()) { it->second.destroy(); terrainMeshes.erase(it); }
        const Chunk* ch = terrain->getChunk(cc);
        if (!ch) return;
        ChunkMesh m = ChunkMesh::build(*ch);
        if (!m.empty()) terrainMeshes.emplace(cc, std::move(m));
    };
    auto rebuildAllTerrain = [&]() {
        for (auto& kv : terrainMeshes) kv.second.destroy();
        terrainMeshes.clear();
        for (const auto& kv : terrain->chunks()) rebuildTerrainChunk(kv.first);
    };
    rebuildAllTerrain();
    for (const auto& kv : bedrock_l->chunks()) {
        ChunkMesh m = ChunkMesh::build(*kv.second);
        if (!m.empty()) bedrockMeshes.emplace(kv.first, std::move(m));
    }

    // ── Camera (fly) ───────────────────────────────────────────────────────────
    // Start in front of the bridge (centered on x, near deck height) looking -Z so
    // the single-thick scene reads as a clean 2D side view.
    float      pitch = -0.18f, yaw = 3.14159265f;  // yaw=π → forward points -Z
    WorldCoord camPos(16.0, 15.0, 26.0);
    double     lastMX = 0, lastMY = 0;
    bool       firstMouse = true, cursorCaptured = true;
    bool       prevF = false, prevLeft = false;
    bool       prevR = false;
    bool       prev0 = false, prev1 = false, prev2 = false;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    Log::info(kLogCat,
        "Structural collapse — fly up to the stone bridge and LEFT-CLICK to mine a "
        "deck voxel near a tower; the unsupported run caves in and the cascade stops "
        "at the bedrock. Keys: 1 crumble, 2 falling-debris, 0 no plugin (cave-in-free), "
        "R rebuild, F cursor, ESC quit.");

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

        // 0/1/2: switch the structural-response plugin live (no engine change).
        const bool cur0 = (glfwGetKey(glfwWin, GLFW_KEY_0) == GLFW_PRESS);
        const bool cur1 = (glfwGetKey(glfwWin, GLFW_KEY_1) == GLFW_PRESS);
        const bool cur2 = (glfwGetKey(glfwWin, GLFW_KEY_2) == GLFW_PRESS);
        if (cur0 && !prev0 && responseMode != 0) setResponse(0);
        if (cur1 && !prev1 && responseMode != 1) setResponse(1);
        if (cur2 && !prev2 && responseMode != 2) setResponse(2);
        prev0 = cur0; prev1 = cur1; prev2 = cur2;

        // R: rebuild the bridge and reset the cascade state.
        const bool curR = (glfwGetKey(glfwWin, GLFW_KEY_R) == GLFW_PRESS);
        if (curR && !prevR) {
            clearDebrisRegion();
            buildDeck();
            physics.reset();                                   // drop old cascade bookkeeping
            physics = std::make_unique<sim::PhysicsSystem>(world, pm);
            rebuildAllTerrain();
            Log::info(kLogCat, "Bridge rebuilt.");
        }
        prevR = curR;

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

        // Fly movement.
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

        // ── Mine on left-click (edge-triggered) ────────────────────────────────
        // One click hollows out the WHOLE 2 m macro the targeted voxel belongs to
        // (its kRatio³ children) — the macro is M13's unit of structure, so this
        // makes "remove a block, the unsupported neighbors cave in" immediate. Each
        // child edit routes through the edit choke point so PropagationSystem sees
        // it; the terrain layer is the only thing raycast picks (bedrock is a
        // separate immutable layer), so the player can never mine the anchors.
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, look, kReachM);
        const bool curLeft = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        if (hit.hit && curLeft && !prevLeft) {
            const VoxelCoord macro = chunkmath::childToParentVoxel(hit.voxel, kRatio);
            const VoxelCoord cmin  = chunkmath::childVoxelMin(macro, kRatio);
            const Voxel empty = Voxel::empty();
            for (int64_t dz = 0; dz < kRatio; ++dz)
                for (int64_t dy = 0; dy < kRatio; ++dy)
                    for (int64_t dx = 0; dx < kRatio; ++dx)
                        nm.applyEdit(kLocalPlayer,
                                     chunkmath::voxelCenter(
                                         {cmin.x + dx, cmin.y + dy, cmin.z + dz}, cvs),
                                     empty);
        }
        prevLeft = curLeft;

        // ── End-of-frame structural pass ───────────────────────────────────────
        // Drains dirty macros, runs the support flood, and fires on_structural_event
        // for newly-unstable macros within budget. The response plugin's edits land
        // through applyEdit above's choke point and re-dirty for the next frame.
        physics->tick();

        // Remesh exactly the terrain chunks any edit (player or plugin) touched.
        for (const ChunkCoord& cc : tracker.touched) rebuildTerrainChunk(cc);
        tracker.touched.clear();

        // ── HUD ────────────────────────────────────────────────────────────────
        const Voxel target = hit.hit ? world.getVoxel(chunkmath::voxelCenter(hit.voxel, cvs))
                                     : Voxel::empty();
        char line0[160];
        std::snprintf(line0, sizeof(line0),
                      "Response: %s  [1 crumble | 2 falling-debris | 0 none | R rebuild]",
                      responseName(responseMode));
        char line1[160];
        std::snprintf(line1, sizeof(line1),
                      "Structural events this frame: %d   carried backlog: %zu   target: %s",
                      physics->eventsFiredLastTick(), physics->carryBacklog(),
                      hit.hit ? "stone" : "-");
        renderer.setHudText({std::string(line0), std::string(line1)});

        if (hit.hit)
            renderer.drawVoxelHighlight(chunkmath::voxelCenter(hit.voxel, cvs),
                                        static_cast<float>(cvs), 0xff00ffff, -1.0f);

        // ── Render ─────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        for (const auto& kv : bedrockMeshes) {
            const Chunk* ch = bedrock_l->getChunk(kv.first);
            if (ch) renderer.renderChunk(kv.second, ch->origin(), bvs, bedrock_l->chunkSizeVoxels());
        }
        for (const auto& kv : terrainMeshes) {
            const Chunk* ch = terrain->getChunk(kv.first);
            if (ch) renderer.renderChunk(kv.second, ch->origin(), cvs, terrain->chunkSizeVoxels());
        }
        renderer.render();
    }

    // Teardown. Drop the physics driver (unregisters its engine hook) and the
    // host's remesh tracker before the plugin manager so no torn-down callback is
    // left dangling.
    physics.reset();
    pm.unregisterEngineVoxelModifiedHook(&tracker);
    for (auto& kv : terrainMeshes) kv.second.destroy();
    for (auto& kv : bedrockMeshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

// M17 demo — HUD and controls.
//
// The reference demo for sanity-check C2: a real, non-trivial game HUD on top of
// the engine's overlay seam, driven entirely by the engine's THREE reference
// plugins so they get a live field test:
//
//   * keyboard-mouse + gamepad input plugins (M17 C1) — all player input flows
//     through their action-mapping / axis / dead-zone API. The host owns the
//     window (and GLFW), so it hands each plugin a tiny RawSource poll adapter;
//     no GLFW type crosses the plugin boundary. The active device auto-switches
//     between keyboard/mouse and a connected gamepad based on which one moved
//     last, so you can pick up a controller mid-session.
//   * kinematic-body plugin (M17 B1) — the player is a kinematic body. The host
//     sets a wish direction + jump each frame; the engine's per-frame tick hook
//     (engine.update) steps gravity + sweep-and-resolve collision against the
//     world. The host reads back position/grounded and derives FALL DAMAGE from
//     the landing speed, feeding the health HUD.
//
// The HUD itself exercises the renderer's cell-grid overlay (BgfxRenderer::hud*,
// M17 C2) — the same bgfx debug-text cells the crosshair uses, exposed as a
// general immediate-mode draw list. It composes four classic HUD elements:
//
//   * Health bar    — depletes on hard landings, regenerates while grounded.
//   * Inventory hotbar — mining a voxel banks its material; the selected slot
//                        places blocks back, consuming from the bank.
//   * Minimap       — a top-down excavation map: as you dig, mined-out columns
//                     read as pits, so the map shows the hole you've carved.
//   * Status line   — coordinates, active input device, fps, selected material.
//
// The world is the flat hardness strata from the material-showcase plugin (M8):
// grass over dirt/stone/iron/diamond over an indestructible bedrock floor, so
// there are several distinct materials to collect and the dig has visible depth.
//
// Controls (keyboard/mouse):  WASD move, mouse look, Space jump, hold LMB mine,
//   RMB place, 1-5 select slot, F toggle cursor, ESC quit.
// Controls (gamepad):  left stick move, right stick look, A jump, RT mine,
//   LT place, bumpers cycle slots.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "simulation/RemovalAccumulator.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include "kinematic_body.h"
#include "keyboard_mouse.h"
#include "gamepad.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

// The reference plugins are compiled into this demo binary (see CMakeLists.txt)
// so their inline-static api() singletons resolve in one address space — the
// same wiring the plugins' tests use. Each exposes a uniquely named init the
// host calls via PluginManager::wireInPlugin.
extern "C" int kinematic_body_plugin_init(PluginContext* ctx);
extern "C" int keyboard_mouse_plugin_init(PluginContext* ctx);
extern "C" int gamepad_plugin_init(PluginContext* ctx);

#ifndef VOXEL_SHOWCASE_PLUGIN_PATH
#  define VOXEL_SHOWCASE_PLUGIN_PATH ""
#endif

namespace {

constexpr char   kLogCat[] = "demo18";
constexpr int    kLoadsPerFrame = 4;      // meshed chunks per frame after spawn
constexpr double kReachM        = 6.0;    // voxel-pick reach in metres
constexpr float  kToolPower     = 1.0f;   // removal work-units/s (M8 RemovalModel)
constexpr double kEyeOffset     = 0.7;    // camera height above the AABB center
constexpr double kPadLookSpeed  = 2.6;    // gamepad look rad/s at full stick
constexpr float  kSafeFallSpeed = 12.0f;  // landing speed below this is harmless
constexpr float  kFallDamageK   = 4.0f;   // HP lost per (m/s) above the safe speed
constexpr float  kRegenPerSec   = 8.0f;   // HP regained per second while grounded
constexpr double kMinimapPeriod = 0.15;   // seconds between minimap rebuilds

// The reference grass surface (material-showcase puts topsoil at world Y 23, so
// its top face is at Y 24). The minimap colors each column by how far its surface
// sits below this — i.e. how deep you've dug there.
constexpr double kSurfaceRefY = 24.0;

constexpr int kMinimapW = 26;
constexpr int kMinimapH = 14;

// Map a material's visual palette index (material-showcase assigns distinct ones)
// to a 4-bit debug-text color so the inventory slot and minimap read in-theme.
uint8_t hudColorForPalette(uint8_t p) {
    switch (p) {
        case 1:  return hud::LightGray;  // stone (gray)
        case 2:  return hud::Green;      // grass
        case 3:  return hud::Brown;      // dirt
        case 10: return hud::DarkGray;   // bedrock (near-black)
        case 11: return hud::LightBlue;  // iron (blue-gray)
        case 13: return hud::LightCyan;  // diamond (cyan)
        default: return hud::White;
    }
}

}  // namespace

int main() {
    // ── Layer config — one flat, walkable, mineable terminal layer (as M8) ──────
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

    // ── Plugins ─────────────────────────────────────────────────────────────────
    // The strata world + materials come from material-showcase, loaded from disk
    // like any content plugin. The three REFERENCE plugins are compiled in and
    // wired through the engine after init (below), since their api() singletons
    // must share this binary's address space.
    World world(terrain);
    Engine engine;
    PluginManager pm;
    engine.init(pm, world);

    if (std::string(VOXEL_SHOWCASE_PLUGIN_PATH).empty()) {
        Log::error(kLogCat, "Fatal: material-showcase plugin path not configured.");
        return 1;
    }
    if (pm.loadPlugin(VOXEL_SHOWCASE_PLUGIN_PATH) == kInvalidPluginId) {
        Log::error(kLogCat, (std::string("Fatal: could not load material-showcase plugin from ")
                             + VOXEL_SHOWCASE_PLUGIN_PATH).c_str());
        return 1;
    }

    // Wire in the reference plugins. kinematic-body registers the per-frame tick
    // hook (engine.update drives it); the input plugins register nothing — their
    // init just fills the shared api() table the host reads below.
    pm.wireInPlugin(kinematic_body_plugin_init);
    pm.wireInPlugin(keyboard_mouse_plugin_init);
    pm.wireInPlugin(gamepad_plugin_init);

    LayerGeneratorFn generator = nullptr;
    void*            generatorUserData = nullptr;
    for (const auto& g : pm.layerGenerators())
        if (g.layer_name == "terrain") { generator = g.fn; generatorUserData = g.user_data; }
    if (!generator) {
        Log::error(kLogCat, "Fatal: no 'terrain' layer generator registered.");
        return 1;
    }

    // ── Inventory model — one slot per registered material ──────────────────────
    struct Slot { std::string id; uint8_t palette; uint8_t color; int count; };
    std::vector<Slot> slots;
    std::unordered_map<uint8_t, size_t> paletteToSlot;  // mined palette idx → slot
    for (const auto& m : pm.materials()) {
        const MaterialProperties props = pm.material(m.material_id);
        paletteToSlot[props.palette_index] = slots.size();
        slots.push_back({m.material_id, props.palette_index,
                         hudColorForPalette(props.palette_index), 0});
        if (slots.size() >= 8) break;
    }
    if (slots.empty()) { Log::error(kLogCat, "Fatal: no materials registered."); return 1; }
    // A starter stack of the topsoil so placing works before you've mined anything.
    slots.front().count = 32;
    size_t selectedSlot = 0;

    // ── Window + renderer ───────────────────────────────────────────────────────
    platform::Window window(1280, 720, "VoxelEngine — M17 HUD and Controls");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);
    engine.setRenderer(&renderer);  // engine metrics: draw-call count → HUD fps line

    LODManager lod(layerConfig);
    lod.setVerticalBand(0, 0);  // flat strata live in the single chunk-Y=0 band

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    auto remeshChunkOf = [&](const chunkmath::VoxelCoord& vc) {
        ChunkCoord cc = chunkmath::voxelToChunkLocal(vc, world.chunkSizeVoxels()).chunk;
        const Chunk* chunk = world.getChunk(cc);
        if (!chunk) return;
        auto it = meshes.find(cc);
        if (it != meshes.end()) { it->second.destroy(); it->second = ChunkMesh::build(*chunk); }
        else meshes.emplace(cc, ChunkMesh::build(*chunk));
    };
    auto editVoxel = [&](const chunkmath::VoxelCoord& vc, const Voxel& newVox) {
        const WorldCoord center = chunkmath::voxelCenter(vc, world.voxelSizeM());
        const Voxel oldVox = world.getVoxel(center);
        if (!world.setVoxel(center, newVox)) return;
        for (const auto& h : pm.voxelModifiedHooks())
            if (h.fn) h.fn(center, &oldVox, &newVox, kLocalPlayer, h.user_data);
        remeshChunkOf(vc);
    };

    // ── Input: install host RawSource adapters into the two input plugins ───────
    // The host owns the window, so these are one-line wrappers over GLFW. The
    // plugins pull raw state through them and do all the mapping themselves.
    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    kbinput::RawSource kbSrc;
    kbSrc.key          = [](int kc, void* u)  { return glfwGetKey((GLFWwindow*)u, kc) == GLFW_PRESS ? 1 : 0; };
    kbSrc.mouse_button = [](int b,  void* u)  { return glfwGetMouseButton((GLFWwindow*)u, b) == GLFW_PRESS ? 1 : 0; };
    kbSrc.cursor       = [](double* x, double* y, void* u) { glfwGetCursorPos((GLFWwindow*)u, x, y); };
    kbSrc.user         = glfwWin;
    kbinput::api().set_source(&kbSrc);

    gpinput::RawSource gpSrc;
    gpSrc.poll = [](int jid, gpinput::GamepadState* out, void*) {
        *out = {};
        GLFWgamepadstate gs;
        if (glfwJoystickIsGamepad(jid) && glfwGetGamepadState(jid, &gs)) {
            out->connected = 1;
            for (int i = 0; i < gpinput::kAxisCount;   ++i) out->axes[i]    = gs.axes[i];
            for (int i = 0; i < gpinput::kButtonCount; ++i) out->buttons[i] = gs.buttons[i];
        }
    };
    gpSrc.user = nullptr;
    gpinput::api().set_source(&gpSrc);

    // Keyboard/mouse bindings: two-key movement axes, named button actions, and
    // number-key slot selection — all rebindable through the plugin.
    kbinput::api().bind_axis("forward", GLFW_KEY_S, GLFW_KEY_W);  // W → +1, S → -1
    kbinput::api().bind_axis("strafe",  GLFW_KEY_A, GLFW_KEY_D);  // D → +1, A → -1
    kbinput::api().bind_key("jump",  GLFW_KEY_SPACE);
    kbinput::api().bind_mouse_button("mine",  GLFW_MOUSE_BUTTON_LEFT);
    kbinput::api().bind_mouse_button("place", GLFW_MOUSE_BUTTON_RIGHT);
    for (size_t i = 0; i < slots.size() && i < 9; ++i)
        kbinput::api().bind_key(("slot" + std::to_string(i + 1)).c_str(),
                                GLFW_KEY_1 + static_cast<int>(i));
    kbinput::api().set_mouse_sensitivity(0.0022);

    // Gamepad bindings: sticks read directly (move/look), buttons/triggers mapped.
    gpinput::api().bind_button("jump",      GLFW_GAMEPAD_BUTTON_A);
    gpinput::api().bind_trigger("mine",     gpinput::AxisRightTrigger);
    gpinput::api().bind_trigger("place",    gpinput::AxisLeftTrigger);
    gpinput::api().bind_button("slot_next", GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
    gpinput::api().bind_button("slot_prev", GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);

    // ── Player (kinematic body) ─────────────────────────────────────────────────
    kinbody::BodyDesc desc;
    desc.center     = WorldCoord(0.5, 27.0, 0.5);  // a few metres above the grass
    desc.eye_offset = kEyeOffset;
    const kinbody::BodyId player = kinbody::api().create_body(&desc);
    if (player == kinbody::kInvalidBody) {
        Log::error(kLogCat, "Fatal: could not create player body.");
        return 1;
    }
    const WorldCoord spawn = desc.center;

    float pitch = -0.15f, yaw = 0.0f;
    WorldCoord camPos = desc.center;
    float  health = 100.0f;
    float  fallSpeed = 0.0f;       // peak downward speed accrued while airborne
    bool   prevGrounded = true;
    bool   cursorCaptured = true;
    bool   prevF = false;
    int    activeDevice = 0;       // 0 = keyboard/mouse, 1 = gamepad

    // ── Pre-stream the spawn neighborhood so the body has ground on frame 1 ─────
    {
        ChunkCoord center = chunkmath::worldToChunk(camPos, world.voxelSizeM(),
                                                    world.chunkSizeVoxels());
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain")) {
            if (meshes.count(c)) continue;
            if (Chunk* chunk = world.loadChunk(c, generator, generatorUserData))
                meshes.emplace(c, ChunkMesh::build(*chunk));
        }
    }

    // Cached minimap cells (glyph, attr per cell), rebuilt on a timer.
    std::vector<uint8_t> minimap(static_cast<size_t>(kMinimapW) * kMinimapH * 2, 0);
    double minimapTimer = 0.0;

    Log::info(kLogCat, "WASD/stick move, mouse/stick look, Space/A jump, LMB/RT mine, "
                       "RMB/LT place, 1-5 or bumpers select slot, F cursor, ESC quit.");

    auto prevTime = std::chrono::high_resolution_clock::now();

    // ── Main loop ───────────────────────────────────────────────────────────────
    while (!window.shouldClose()) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        window.pollEvents();
        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // F toggles cursor capture — a window concern the host keeps, not a mapped
        // game action. Re-baseline mouse-look on re-capture so the cursor jump is
        // swallowed by the plugin.
        const bool curF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curF && !prevF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            kbinput::api().set_source(&kbSrc);  // re-baseline the mouse delta
        }
        prevF = curF;

        // Pump both input plugins (edges + deltas advance for both regardless of
        // which is active, so a hot-swap mid-press is clean).
        kbinput::api().update(dt);
        gpinput::api().update(dt);

        double padLX, padLY, padRX, padRY;
        gpinput::api().stick(gpinput::StickLeft,  &padLX, &padLY);
        gpinput::api().stick(gpinput::StickRight, &padRX, &padRY);
        double mdx, mdy;
        kbinput::api().mouse_delta(&mdx, &mdy);

        // Auto-select the active device from this frame's activity.
        const bool padActive = gpinput::api().connected() &&
            (std::abs(padLX) + std::abs(padLY) + std::abs(padRX) + std::abs(padRY) > 0.2 ||
             gpinput::api().held("jump") || gpinput::api().held("mine") ||
             gpinput::api().held("place"));
        const bool kbActive =
            (mdx != 0.0 || mdy != 0.0) ||
            kbinput::api().axis("forward") != 0.0 || kbinput::api().axis("strafe") != 0.0 ||
            kbinput::api().held("jump") || kbinput::api().held("mine") ||
            kbinput::api().held("place");
        if (padActive)      activeDevice = 1;
        else if (kbActive)  activeDevice = 0;

        // ── Look ────────────────────────────────────────────────────────────────
        if (activeDevice == 1) {
            yaw   += static_cast<float>(padRX * kPadLookSpeed * dt);
            pitch -= static_cast<float>(padRY * kPadLookSpeed * dt);
        } else if (cursorCaptured) {
            yaw   += static_cast<float>(mdx);
            pitch -= static_cast<float>(mdy);
        }
        pitch = std::max(-1.55f, std::min(1.55f, pitch));

        // ── Movement wish (world space, from camera yaw) ────────────────────────
        double fwd = 0.0, strafe = 0.0;
        if (activeDevice == 1) { fwd = -padLY; strafe = padLX; }
        else { fwd = kbinput::api().axis("forward"); strafe = kbinput::api().axis("strafe"); }

        const double sy = std::sin(yaw), cy = std::cos(yaw);
        const glm::dvec3 fwdH(sy, 0.0, cy), rightH(cy, 0.0, -sy);
        glm::dvec3 wish = fwdH * fwd + rightH * strafe;

        const bool jump = (activeDevice == 1) ? gpinput::api().pressed("jump")
                                              : kbinput::api().pressed("jump");

        kinbody::BodyInput in;
        in.wish_x = wish.x; in.wish_y = 0.0; in.wish_z = wish.z;
        in.jump   = jump;
        kinbody::api().set_input(player, &in);

        // ── Step the kinematic body (engine.update fires its tick hook) ─────────
        engine.update(dt);

        const kinbody::BodyState* st = kinbody::api().get_state(player);
        const WorldCoord playerCenter = st ? st->center : spawn;
        const bool grounded = st && st->grounded;

        // Fall damage: track peak descent speed while airborne; on landing, convert
        // the excess over the safe speed into HP loss.
        if (st) {
            // Gravity points -Y, so a descent gives vel_y < 0; track the peak
            // downward speed as the positive magnitude -vel_y.
            if (!grounded) fallSpeed = std::max(fallSpeed, static_cast<float>(-st->vel_y));
            if (grounded && !prevGrounded && fallSpeed > kSafeFallSpeed)
                health = std::max(0.0f, health - (fallSpeed - kSafeFallSpeed) * kFallDamageK);
            if (grounded) fallSpeed = 0.0f;
        }
        prevGrounded = grounded;
        if (grounded && health < 100.0f) health = std::min(100.0f, health + kRegenPerSec * dt);

        // Respawn on death or on falling out of the world.
        if (health <= 0.0f || playerCenter.value.y < -8.0) {
            kinbody::api().set_position(player, spawn);
            health = 100.0f; fallSpeed = 0.0f; prevGrounded = true;
        }

        camPos = WorldCoord(playerCenter.value + glm::dvec3(0.0, kEyeOffset, 0.0));

        // ── Slot selection ──────────────────────────────────────────────────────
        if (activeDevice == 1) {
            if (gpinput::api().pressed("slot_next"))
                selectedSlot = (selectedSlot + 1) % slots.size();
            if (gpinput::api().pressed("slot_prev"))
                selectedSlot = (selectedSlot + slots.size() - 1) % slots.size();
        } else {
            for (size_t i = 0; i < slots.size() && i < 9; ++i)
                if (kbinput::api().pressed(("slot" + std::to_string(i + 1)).c_str()))
                    selectedSlot = i;
        }

        // ── Stream chunks around the player ─────────────────────────────────────
        ChunkCoord center = chunkmath::worldToChunk(camPos, world.voxelSizeM(),
                                                    world.chunkSizeVoxels());
        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain")) {
            if (meshes.count(c)) continue;
            if (Chunk* chunk = world.loadChunk(c, generator, generatorUserData)) {
                meshes.emplace(c, ChunkMesh::build(*chunk));
                if (++loaded >= kLoadsPerFrame) break;
            }
        }
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, "terrain") && !world.isChunkDirty(kv.first))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& c : toEvict) {
            meshes[c].destroy(); meshes.erase(c); world.unloadChunk(c);
        }

        // ── Targeting / mine / place ────────────────────────────────────────────
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const glm::dvec3 look(cp * sy, sp, cp * cy);
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, look, kReachM);

        const bool mine  = (activeDevice == 1) ? gpinput::api().held("mine")
                                               : kbinput::api().held("mine");
        const bool place = (activeDevice == 1) ? gpinput::api().pressed("place")
                                               : kbinput::api().pressed("place");

        static sim::RemovalAccumulator remover;  // held-to-mine progress (M8)
        const Voxel target =
            hit.hit ? world.getVoxel(chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()))
                    : Voxel::empty();

        if (hit.hit && mine) {
            if (remover.accrue(hit.voxel, target.material.hardness, kToolPower, dt)) {
                auto it = paletteToSlot.find(target.material.palette_index);
                if (it != paletteToSlot.end()) ++slots[it->second].count;  // bank it
                editVoxel(hit.voxel, Voxel::empty());
                remover.reset();
            }
        } else {
            remover.reset();
        }

        if (hit.hit && place && slots[selectedSlot].count > 0) {
            const chunkmath::VoxelCoord t = hit.adjacent;
            // Don't place inside the player's own AABB.
            const double vs = world.voxelSizeM();
            glm::dvec3 cmin{t.x * vs, t.y * vs, t.z * vs};
            glm::dvec3 cmax = cmin + glm::dvec3(vs, vs, vs);
            glm::dvec3 pmin = playerCenter.value - glm::dvec3(0.3, 0.9, 0.3);
            glm::dvec3 pmax = playerCenter.value + glm::dvec3(0.3, 0.9, 0.3);
            const bool overlapsPlayer =
                pmin.x < cmax.x && pmax.x > cmin.x && pmin.y < cmax.y &&
                pmax.y > cmin.y && pmin.z < cmax.z && pmax.z > cmin.z;
            if (!overlapsPlayer) {
                Voxel placed; placed.material = pm.material(slots[selectedSlot].id);
                editVoxel(t, placed);
                --slots[selectedSlot].count;
            }
        }

        if (hit.hit) {
            const float progress =
                (remover.hasTarget() && remover.target() == hit.voxel) ? remover.progress() : -1.0f;
            renderer.drawVoxelHighlight(
                chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()),
                static_cast<float>(world.voxelSizeM()), 0xff00ffff, progress);
        }

        // ── Minimap (top-down excavation map), rebuilt on a timer ───────────────
        minimapTimer -= dt;
        if (minimapTimer <= 0.0) {
            minimapTimer = kMinimapPeriod;
            const int px = static_cast<int>(std::floor(playerCenter.value.x));
            const int pz = static_cast<int>(std::floor(playerCenter.value.z));
            const int topY = static_cast<int>(std::floor(playerCenter.value.y)) + 6;
            for (int cy2 = 0; cy2 < kMinimapH; ++cy2) {
                for (int cx = 0; cx < kMinimapW; ++cx) {
                    const int wx = px + (cx - kMinimapW / 2);
                    const int wz = pz + (cy2 - kMinimapH / 2);
                    int surfaceTop = -1000;
                    for (int wy = topY; wy >= 0; --wy) {
                        if (!world.getVoxel(WorldCoord(wx + 0.5, wy + 0.5, wz + 0.5)).isEmpty()) {
                            surfaceTop = wy + 1; break;
                        }
                    }
                    uint8_t glyph = ' ', color;
                    if (surfaceTop == -1000) { color = hud::Black; }
                    else {
                        const double d = surfaceTop - kSurfaceRefY;  // <0 == dug down
                        if      (d <= -8.0) color = hud::Blue;
                        else if (d <= -4.0) color = hud::LightBlue;
                        else if (d <= -1.0) color = hud::Cyan;
                        else if (d <=  0.0) color = hud::Green;
                        else                color = hud::Yellow;
                    }
                    const size_t idx = (static_cast<size_t>(cy2) * kMinimapW + cx) * 2;
                    minimap[idx]     = glyph;
                    minimap[idx + 1] = hud::attr(hud::White, color);
                }
            }
        }

        // ── HUD ─────────────────────────────────────────────────────────────────
        const EngineMetrics metrics = engine.getMetrics();
        renderer.hudClear();

        // Health bar (top-left).
        {
            const int barW = 20;
            const int filled = static_cast<int>(std::round(health / 100.0f * barW));
            const uint8_t fullColor = health > 30.0f ? hud::LightGreen : hud::LightRed;
            renderer.hudText(1, 1, hud::attr(hud::White), "HP");
            renderer.hudFill(4, 1, filled, 1, hud::attr(hud::Black, fullColor));
            renderer.hudFill(4 + filled, 1, barW - filled, 1, hud::attr(hud::Black, hud::DarkGray));
            char hp[16]; std::snprintf(hp, sizeof(hp), " %3d", static_cast<int>(health));
            renderer.hudText(4 + barW, 1, hud::attr(hud::White), hp);
        }

        // Minimap (top-right), then the player marker + heading on top.
        const int mmCol = renderer.hudCols() - kMinimapW - 1;
        const int mmRow = 1;
        if (mmCol > 30) {
            renderer.hudCells(mmCol, mmRow, kMinimapW, kMinimapH, minimap.data());
            const int pcx = mmCol + kMinimapW / 2;
            const int pcy = mmRow + kMinimapH / 2;
            renderer.hudText(pcx, pcy, hud::attr(hud::White, hud::Magenta), "@");
            const int hx = static_cast<int>(std::lround(std::sin(static_cast<double>(yaw))));
            const int hz = static_cast<int>(std::lround(std::cos(static_cast<double>(yaw))));
            renderer.hudText(pcx + hx, pcy + hz, hud::attr(hud::Yellow), "*");
            renderer.hudText(mmCol, mmRow + kMinimapH, hud::attr(hud::LightGray), "MAP");
        }

        // Inventory hotbar (bottom-center): one colored slot per material.
        {
            const int slotW = 7;
            const int totalW = static_cast<int>(slots.size()) * slotW;
            const int startCol = std::max(1, (renderer.hudCols() - totalW) / 2);
            const int row = renderer.hudRows() - 3;
            for (size_t i = 0; i < slots.size(); ++i) {
                const int x = startCol + static_cast<int>(i) * slotW;
                const bool sel = (i == selectedSlot);
                renderer.hudText(x, row - 1, hud::attr(hud::LightGray),
                                 std::to_string(i + 1));
                char cell[16];
                std::snprintf(cell, sizeof(cell), "%c%-4d",
                              static_cast<char>(std::toupper(slots[i].id[0])), slots[i].count);
                const uint8_t a = sel ? hud::attr(hud::Black, hud::Yellow)
                                      : hud::attr(hud::White, slots[i].color);
                renderer.hudText(x, row, a, cell);
            }
            const std::string hint = "[" + slots[selectedSlot].id + "]";
            renderer.hudText(startCol, row + 1, hud::attr(hud::Yellow), hint);
        }

        // Status line (bottom).
        {
            char status[160];
            std::snprintf(status, sizeof(status),
                "pos %.0f,%.0f,%.0f | %s | %s | %.0f fps | LMB/RT mine  RMB/LT place",
                playerCenter.value.x, playerCenter.value.y, playerCenter.value.z,
                activeDevice == 1 ? "GAMEPAD" : "KBD/MOUSE",
                grounded ? "grounded" : "airborne",
                metrics.frameTimeSec > 0.0 ? 1.0 / metrics.frameTimeSec : 0.0);
            renderer.hudText(1, renderer.hudRows() - 1, hud::attr(hud::White), status);
        }

        // ── Render ──────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }
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

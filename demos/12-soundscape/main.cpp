// M12 demo — soundscape.
//
// The audio sibling of the M8 "material matters" world. The same flat strata
// world (grass over dirt, stone, iron, diamond on an indestructible bedrock
// floor) built by the `material-showcase` plugin, but here every interaction
// makes a MATERIAL-APPROPRIATE POSITIONAL SOUND:
//
//   • Footsteps  — the demo's kinematic body fires play_material_sound(Footstep,
//                  ground_palette_index, footPos) from the voxel under the
//                  player's feet, on a stride cadence. No hardcoded surface
//                  table: the sound is selected by the ground voxel's own
//                  palette_index (ARCHITECTURE §16).
//   • Break/Place — supplied by the removable `material-audio` plugin off the
//                  existing on_voxel_modified hook (no engine change). Dropping
//                  that plugin removes break/place audio entirely.
//   • Ambient bed — a looping create_emitter source pinned at a WorldCoord that
//                  is re-projected to camera-local space every frame, so it PANS
//                  correctly as the listener moves (the floating-origin audio
//                  rule: the listener is the local origin, sources are fed
//                  toLocalFloat(camera), §1/§9/§16).
//
// The listener is pushed from the camera every frame via AudioManager::setListener
// — the audio analog of how the renderer receives the camera. A HUD line reads out
// the active voice count and the last sound's event/material, so the spatial +
// material path is directly visible.
//
// Controls: WASD move, mouse look, F toggles cursor, G toggles walk/fly,
// Space/Shift up/down (fly) or jump (walk), left mouse breaks, right mouse places,
// 1-6 select material, ESC quits. The material-audio plugin resolves its WAV
// assets under assets/audio/ relative to the working directory; if that folder is
// not in the cwd the demo switches to the source-tree root (VOXEL_REPO_ROOT) at
// startup, so it plays correctly whether launched from the repo root or build/.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/VoxelCollision.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include "audio/AudioManager.h"
#include "audio/AudioValidation.h"
#include "audio/MiniaudioBackend.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VOXEL_SHOWCASE_PLUGIN_PATH
#  define VOXEL_SHOWCASE_PLUGIN_PATH ""
#endif
#ifndef VOXEL_MATERIAL_AUDIO_PLUGIN_PATH
#  define VOXEL_MATERIAL_AUDIO_PLUGIN_PATH ""
#endif

namespace {
constexpr int    kLoadsPerFrame = 2;      // budget generated/meshed chunks per frame
constexpr float  kFlySpeed      = 18.0f;  // free-fly camera speed
constexpr float  kMouseSens     = 0.002f;
constexpr double kReachM        = 8.0;    // how far the player can target a voxel

// Walking player (walk mode).
constexpr double kWalkSpeed = 6.0;        // m/s horizontal
constexpr double kGravity   = 25.0;       // m/s^2
constexpr double kJumpSpeed = 8.0;        // m/s initial upward
constexpr double kEyeOffset = 0.7;        // eye height above the AABB center
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);  // half-extents: 0.6 wide, 1.8 tall

constexpr double kStrideSeconds = 0.42;   // footstep cadence while walking
constexpr double kPi            = 3.14159265358979323846;  // M_PI is not portable (MSVC)

// Ambient bed: a looping source pinned just above the surface near spawn. Audible
// across the small play area but still pans/attenuates as the listener moves.
const WorldCoord kAmbientPos(0.0, 24.0, 0.0);
const char*      kAmbientSoundId = "ambient_bed";
const char*      kAmbientWavPath = "ambient_bed.wav";  // generated below, cwd-relative

const char* audioEventName(AudioEvent e) {
    switch (e) {
        case AudioEvent::Footstep: return "Footstep";
        case AudioEvent::Break:    return "Break";
        case AudioEvent::Place:    return "Place";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Ambient asset generation.
//
// The strata world has no bundled ambient track, so the demo synthesises a small
// seamless-looping mono drone the first time it runs (the same "generate the
// demo's own asset" pattern the M7 round-trip demo uses for test_model.vox). The
// loop length is an integer number of periods of every partial and of the slow
// amplitude LFO, so it repeats without a click.
// ---------------------------------------------------------------------------
bool ensureAmbientWav(const std::string& path) {
    {
        std::ifstream probe(path, std::ios::binary);
        if (probe.good()) return true;  // already present from a previous run
    }

    const int   sampleRate = 44100;
    const double seconds    = 4.0;                 // 4 s loop; 1/seconds = 0.25 Hz grid
    const int   frames      = static_cast<int>(sampleRate * seconds);

    // Low, calm partials — all integer multiples of 55 Hz, so each completes a
    // whole number of cycles over the 4 s loop (55*4 = 220 cycles, etc.).
    struct Partial { double freq; double amp; };
    const Partial partials[] = {
        { 55.0, 0.45 }, { 110.0, 0.30 }, { 165.0, 0.16 }, { 220.0, 0.09 },
    };

    std::vector<int16_t> pcm(static_cast<size_t>(frames));
    double peak = 0.0;
    std::vector<double> raw(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        // Slow amplitude swell, one full cycle per loop (0.25 Hz) — seamless.
        const double lfo = 0.65 + 0.35 * std::sin(2.0 * kPi * 0.25 * t - kPi * 0.5);
        double s = 0.0;
        for (const auto& p : partials)
            s += p.amp * std::sin(2.0 * kPi * p.freq * t);
        s *= lfo;
        raw[static_cast<size_t>(i)] = s;
        peak = std::max(peak, std::fabs(s));
    }
    const double norm = peak > 0.0 ? (0.5 / peak) : 0.0;  // headroom: peak ~0.5
    for (int i = 0; i < frames; ++i)
        pcm[static_cast<size_t>(i)] =
            static_cast<int16_t>(raw[static_cast<size_t>(i)] * norm * 32767.0);

    // 16-bit mono PCM WAV. Mono so miniaudio's spatializer positions it in 3D.
    const int channels      = 1;
    const int bitsPerSample = 16;
    const int blockAlign    = channels * (bitsPerSample / 8);
    const int byteRate      = sampleRate * blockAlign;
    const int dataSize      = frames * blockAlign;

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w16 = [&](uint16_t v) { f.put(char(v & 0xff)); f.put(char((v >> 8) & 0xff)); };
    auto w32 = [&](uint32_t v) { w16(uint16_t(v & 0xffff)); w16(uint16_t((v >> 16) & 0xffff)); };
    f.write("RIFF", 4);             w32(uint32_t(36 + dataSize));  f.write("WAVE", 4);
    f.write("fmt ", 4);             w32(16); w16(1); w16(uint16_t(channels));
    w32(uint32_t(sampleRate));      w32(uint32_t(byteRate));
    w16(uint16_t(blockAlign));      w16(uint16_t(bitsPerSample));
    f.write("data", 4);             w32(uint32_t(dataSize));
    f.write(reinterpret_cast<const char*>(pcm.data()),
            static_cast<std::streamsize>(pcm.size() * sizeof(int16_t)));
    return f.good();
}

}  // namespace

int main() {
    // The material-audio plugin resolves its sound assets relative to the working
    // directory (the documented "assets/audio/" convention, like voxelsave/). If
    // the demo is launched from somewhere without assets/audio (e.g. the build
    // output dir), fall back to the source-tree root injected at build time so the
    // material cues still load — otherwise only the demo-generated ambient bed
    // (written into the cwd) would play. Done before anything reads a relative path.
#ifdef VOXEL_REPO_ROOT
    if (!std::filesystem::exists("assets/audio")) {
        std::error_code ec;
        std::filesystem::current_path(VOXEL_REPO_ROOT, ec);
        if (ec)
            std::cerr << "[main] Warning: 'assets/audio' not in the working directory "
                         "and could not switch to " VOXEL_REPO_ROOT " — material sounds "
                         "may be silent. Run from a directory containing assets/audio/.\n";
        else
            std::cout << "[main] Working directory set to " VOXEL_REPO_ROOT
                         " so assets/audio/ resolves.\n";
    }
#endif

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

    // ── Plugins ──────────────────────────────────────────────────────────
    // material-showcase MUST load first: it registers the strata materials, and
    // material-audio resolves material_id → palette_index at registration time.
    PluginManager pluginManager;
    if (std::string(VOXEL_SHOWCASE_PLUGIN_PATH).empty() ||
        pluginManager.loadPlugin(VOXEL_SHOWCASE_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load material-showcase plugin from '"
                  << VOXEL_SHOWCASE_PLUGIN_PATH << "'\n";
        return 1;
    }
    // material-audio is the removable default break/place audio (off on_voxel_modified)
    // plus the example sound assets and per-material bindings. Optional — the demo
    // still runs (silently for edits) if it is missing.
    if (std::string(VOXEL_MATERIAL_AUDIO_PLUGIN_PATH).empty() ||
        pluginManager.loadPlugin(VOXEL_MATERIAL_AUDIO_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Warning: material-audio plugin not loaded from '"
                  << VOXEL_MATERIAL_AUDIO_PLUGIN_PATH
                  << "' — break/place/footstep audio will be silent.\n";
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
        std::cerr << "[main] Fatal: no 'terrain' layer generator registered.\n";
        return 1;
    }

    // Build-material palette (keys 1-N) and a palette_index → name reverse map so
    // the HUD can name the material behind any sound (read off the voxel's own
    // palette_index, never a block-type id — ARCHITECTURE §5/§16).
    std::vector<std::string>             buildMaterials;
    std::unordered_map<uint8_t, std::string> paletteToName;
    for (const auto& m : pluginManager.materials()) {
        paletteToName[m.props.palette_index] = m.material_id;
        if (buildMaterials.size() < 9) buildMaterials.push_back(m.material_id);
    }
    if (buildMaterials.empty()) {
        std::cerr << "[main] Fatal: no materials registered by the plugin.\n";
        return 1;
    }
    size_t selectedMaterial = 0;
    auto nameForPalette = [&](uint8_t idx) -> std::string {
        auto it = paletteToName.find(idx);
        return it != paletteToName.end() ? it->second : ("#" + std::to_string(idx));
    };

    // ── Audio ────────────────────────────────────────────────────────────
    // Real device (not the null device the tests use). If it fails to init, the
    // demo continues visually with audio disabled — audio is a pure sink (§4).
    audio::MiniaudioBackend backend(/*useNullDevice=*/false);
    audio::AudioManager     audioManager(&backend, pluginManager);
    pluginManager.setAudioManager(&audioManager);  // route plugin play_* calls here

    if (backend.isReady()) {
        audioManager.preloadSounds();  // load every register_sound asset into the backend
        // Startup validation: surface a dangling binding or unloadable asset. Warn
        // (not Error) so a missing optional sound never refuses to run the demo.
        audio::validateAudio(pluginManager, &backend, audio::AudioStrictPolicy::Warn);
    } else {
        std::cerr << "[main] Warning: audio backend unavailable — running silently.\n";
    }

    // Ambient bed: synthesise the loop, load it straight into the backend (a
    // front-end-owned asset, not a plugin registration), and spawn one looping
    // emitter pinned in world space so it pans as the listener moves.
    AudioEmitterId ambientEmitter = kInvalidEmitterId;
    if (backend.isReady()) {
        if (ensureAmbientWav(kAmbientWavPath)) {
            SoundParams ap{};
            ap.volume       = 0.5f;
            ap.min_distance = 8.0f;    // full volume out to 8 m, then attenuates
            ap.max_distance = 80.0f;   // still audible across the small world
            if (backend.loadSound(kAmbientSoundId, kAmbientWavPath, ap)) {
                EmitterParams ep{};
                ep.sound = ap;
                ep.loop  = true;
                ambientEmitter =
                    audioManager.createEmitter(kAmbientSoundId, kAmbientPos, ep);
            }
        }
        if (ambientEmitter == kInvalidEmitterId)
            std::cerr << "[main] Warning: ambient bed could not start.\n";
    }

    Engine engine;
    engine.start();  // background tick thread (audio is driven from this loop, below)

    platform::Window window(1024, 768, "VoxelEngine — M12 Soundscape");

    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    World world(terrain);
    LODManager lod(layerConfig);
    lod.setVerticalBand(0, 0);

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    // Last-sound HUD state, updated wherever the demo triggers a sound.
    std::string lastSoundLabel = "(none)";
    auto recordSound = [&](AudioEvent e, uint8_t palette) {
        lastSoundLabel = std::string(audioEventName(e)) + " " + nameForPalette(palette);
    };

    // (Re)build the GPU mesh for the chunk owning a just-edited voxel.
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

    // Apply an edit: write it, fire on_voxel_modified (material-audio plays the
    // Break/Place sound off this hook), re-mesh, and record the HUD sound label.
    auto editVoxel = [&](const chunkmath::VoxelCoord& vc, const Voxel& newVox) {
        const WorldCoord center = chunkmath::voxelCenter(vc, world.voxelSizeM());
        const Voxel oldVox = world.getVoxel(center);
        if (!world.setVoxel(center, newVox)) return;
        for (const auto& h : pluginManager.voxelModifiedHooks())
            if (h.fn) h.fn(center, &oldVox, &newVox, kLocalPlayer, h.user_data);
        if (!oldVox.isEmpty()) recordSound(AudioEvent::Break, oldVox.material.palette_index);
        if (!newVox.isEmpty()) recordSound(AudioEvent::Place, newVox.material.palette_index);
        remeshChunkOf(vc);
    };

    // Camera: start above the strata surface (topsoil at y=23), looking down.
    float      pitch = -0.5f, yaw = 0.0f;
    WorldCoord camPos(0.0, 30.0, 0.0);
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true;
    bool       cursorCaptured = true;
    bool       prevKeyF = false, prevKeyG = false;
    bool       prevLeft = false, prevRight = false;

    // Walking player state (active when walkMode is true).
    bool       walkMode = false;
    WorldCoord playerCenter(0.0, 0.0, 0.0);
    double     vy = 0.0;
    bool       grounded = false;
    double     strideTimer = 0.0;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    std::cout << "[main] Soundscape — walk (G) and build to hear material-appropriate\n"
                 "[main] positional sounds over a panning ambient bed. Left mouse breaks,\n"
                 "[main] right mouse places, 1-" << buildMaterials.size()
              << " selects, F = cursor, ESC quits.\n";

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
                vy = 0.0;
                grounded = false;
                strideTimer = 0.0;
            }
            std::cout << "[main] Mode: " << (walkMode ? "WALK (footsteps on)"
                                                      : "FLY") << "\n";
        }
        prevKeyG = curKeyG;

        for (int i = 0; i < static_cast<int>(buildMaterials.size()); ++i) {
            if (glfwGetKey(glfwWin, GLFW_KEY_1 + i) == GLFW_PRESS &&
                selectedMaterial != static_cast<size_t>(i)) {
                selectedMaterial = static_cast<size_t>(i);
                std::cout << "[main] Selected material: "
                          << buildMaterials[selectedMaterial] << "\n";
            }
        }

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

        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);

        if (!walkMode) {
            glm::dvec3 fwd  {static_cast<double>(cp * sy), static_cast<double>(sp),
                             static_cast<double>(cp * cy)};
            glm::dvec3 right{static_cast<double>(cy), 0.0, static_cast<double>(-sy)};
            glm::dvec3 delta{0.0, 0.0, 0.0};
            double step = static_cast<double>(kFlySpeed * dt);
            if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += fwd   * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= fwd   * step;
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
            const bool moving = glm::length(wish) > 0.0;
            if (moving) wish = glm::normalize(wish);

            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE) == GLFW_PRESS && grounded)
                vy = kJumpSpeed;
            vy -= kGravity * static_cast<double>(dt);

            glm::dvec3 delta = wish * (kWalkSpeed * static_cast<double>(dt));
            delta.y += vy * static_cast<double>(dt);

            voxelcollide::MoveResult mr =
                voxelcollide::moveAABB(world, {playerCenter, kPlayerHalf}, delta);
            playerCenter = mr.position;
            grounded = mr.grounded;
            if (mr.grounded || (mr.hitY && vy > 0.0)) vy = 0.0;
            camPos = WorldCoord(playerCenter.value + glm::dvec3(0.0, kEyeOffset, 0.0));

            // Footsteps: on a stride cadence while grounded and moving, play the
            // sound of the voxel under the feet. Material is read off that voxel's
            // own palette_index — no surface table (ARCHITECTURE §16).
            if (grounded && moving) {
                strideTimer += static_cast<double>(dt);
                if (strideTimer >= kStrideSeconds) {
                    strideTimer = 0.0;
                    const glm::dvec3 feet = playerCenter.value -
                                            glm::dvec3(0.0, kPlayerHalf.y + 0.05, 0.0);
                    const Voxel ground = world.getVoxel(WorldCoord(feet));
                    if (!ground.isEmpty()) {
                        audioManager.playMaterialSound(
                            AudioEvent::Footstep, ground.material.palette_index,
                            WorldCoord(feet));
                        recordSound(AudioEvent::Footstep, ground.material.palette_index);
                    }
                }
            } else {
                strideTimer = kStrideSeconds;  // first step on resuming fires promptly
            }
        }

        // ── Listener: push the camera to the audio system every frame ─────
        // forward from pitch/yaw, world-up = +Y. The listener stays pinned at the
        // local origin inside the backend; sources are re-projected relative to it.
        if (backend.isReady()) {
            glm::vec3 fwd{cp * sy, sp, cp * cy};
            audioManager.setListener(camPos, fwd, glm::vec3(0.0f, 1.0f, 0.0f));
        }

        // ── Stream chunks around the camera ──────────────────────────────
        ChunkCoord center =
            chunkmath::worldToChunk(camPos, world.voxelSizeM(), world.chunkSizeVoxels());

        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain")) {
            if (meshes.count(c)) continue;
            Chunk* chunk = world.loadChunk(c, generator, generatorUserData);
            if (!chunk) continue;
            meshes.emplace(c, ChunkMesh::build(*chunk));
            if (++loaded >= kLoadsPerFrame) break;
        }

        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, "terrain") && !world.isChunkDirty(kv.first))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& c : toEvict) {
            meshes[c].destroy();
            meshes.erase(c);
            world.unloadChunk(c);
        }

        // ── Targeting and edits ──────────────────────────────────────────
        glm::dvec3 lookDir{static_cast<double>(cp * sy), static_cast<double>(sp),
                           static_cast<double>(cp * cy)};
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);

        bool curLeft  = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS);
        bool curRight = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        const Voxel target =
            hit.hit ? world.getVoxel(chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()))
                    : Voxel::empty();

        // Break on left-click edge — but never the indestructible bedrock floor
        // (hardness < 0), so the player can't fall into the void (and bedrock has
        // no break sound bound, demonstrating fail-soft resolution).
        if (hit.hit && curLeft && !prevLeft && target.material.hardness >= 0.0f) {
            editVoxel(hit.voxel, Voxel::empty());
        }
        prevLeft = curLeft;

        // Place on right-click edge into the adjacent empty cell, guarded against
        // placing into the cell the player occupies.
        if (hit.hit && curRight && !prevRight) {
            const double vs = world.voxelSizeM();
            const chunkmath::VoxelCoord t = hit.adjacent;
            bool blockedByPlayer;
            if (walkMode) {
                glm::dvec3 cmin{static_cast<double>(t.x) * vs,
                                static_cast<double>(t.y) * vs,
                                static_cast<double>(t.z) * vs};
                glm::dvec3 cmax = cmin + glm::dvec3(vs, vs, vs);
                glm::dvec3 pmin = playerCenter.value - kPlayerHalf;
                glm::dvec3 pmax = playerCenter.value + kPlayerHalf;
                blockedByPlayer = (pmin.x < cmax.x && pmax.x > cmin.x &&
                                   pmin.y < cmax.y && pmax.y > cmin.y &&
                                   pmin.z < cmax.z && pmax.z > cmin.z);
            } else {
                blockedByPlayer = (t == chunkmath::worldToVoxel(camPos, vs));
            }
            if (!blockedByPlayer) {
                Voxel placed;
                placed.material = pluginManager.material(buildMaterials[selectedMaterial]);
                editVoxel(t, placed);
            }
        }
        prevRight = curRight;

        // ── Audio tick: re-project emitters to camera-local, prune one-shots ──
        // Driven here on the main thread (not via Engine::setAudioManager) so all
        // audio mutation stays single-threaded (§16 keeps audio a pure sink).
        if (backend.isReady()) audioManager.update();

        // ── HUD ───────────────────────────────────────────────────────────
        char line0[96];
        std::snprintf(line0, sizeof(line0), "Voices: %zu   Last sound: %s",
                      backend.isReady() ? audioManager.activeVoiceCount() : size_t(0),
                      lastSoundLabel.c_str());
        char line1[96];
        std::snprintf(line1, sizeof(line1), "Mode %s | Build: %s | Target: %s",
                      walkMode ? "WALK" : "FLY",
                      buildMaterials[selectedMaterial].c_str(),
                      hit.hit ? nameForPalette(target.material.palette_index).c_str()
                              : "-");
        renderer.setHudText({std::string(line0), std::string(line1)});

        if (hit.hit) {
            renderer.drawVoxelHighlight(
                chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()),
                static_cast<float>(world.voxelSizeM()), 0xff00ffff, -1.0f);
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

    // Teardown. Stop the demo-owned ambient emitter, then drop the engine's view
    // of the audio manager before any audio object is destroyed so plugin unload
    // (in PluginManager's destructor) never calls into a dead AudioManager.
    if (ambientEmitter != kInvalidEmitterId) audioManager.stopEmitter(ambientEmitter);
    pluginManager.setAudioManager(nullptr);

    for (auto& kv : meshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

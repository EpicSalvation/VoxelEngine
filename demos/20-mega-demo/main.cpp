// M18 demo — the Mega-Demo ("Overworld"): a Minecraft-lite survival slice that
// exercises as much of the engine as one coherent world can.
//
// One playable scene wires together, end to end:
//   • Seeded worldgen (M3/M8) — a rolling heightmap of grass/dirt/stone/sand with
//     buried caves and iron ore, built by the `overworld` plugin's terminal layer
//     generator plus its cave/ore feature overlays. The world SEED is a launch
//     argument (argv[1], deterministic default when omitted) threaded through the
//     generator and the feature pass, so the SAME seed regenerates the SAME world
//     (the §4 determinism guarantee) — shown live on the HUD.
//   • Trees (M4 composition) — the separate `trees` plugin stamps trunks+canopy
//     onto the surface as another feature overlay: cross-plugin composition with
//     no shared code. Drop the plugin and the world is treeless.
//   • Water (M14 material) — the `water` plugin floods valleys below sea level.
//   • Textured voxels (M15) — the overworld materials bind per-face atlas tiles;
//     the demo synthesises the PNG tiles and builds the atlas at startup.
//   • A zombie-like MOB (new this milestone) — the `mob` plugin runs a
//     wander/chase/attack AI on the engine's per-frame tick + move_aabb seams; the
//     demo renders each body (the engine has no entity system) and the bites drain
//     the player's health.
//   • Player + HUD + input (M17) — a kinematic body driven by the keyboard-mouse
//     and gamepad reference plugins, mine/place, fall damage, and a cell-grid HUD.
//   • Positional + ambient AUDIO (M12) — a panning nature bed, per-mob growls, and
//     material-appropriate footstep/break/place cues.
//   • Distance fog (M16) for the LOD sell.
//
// Controls: WASD/stick move, mouse/stick look, Space/A jump, LMB/RT mine,
// RMB/LT place, 1-8 select material, F toggles cursor, ESC quits.
//
// Run `20-mega-demo 12345` twice → identical worlds; a different number → a
// different world; no argument → the deterministic default.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "renderer/TextureManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/VoxelCollision.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include "audio/AudioManager.h"
#include "audio/AudioValidation.h"
#include "audio/MiniaudioBackend.h"

#include "net/NetworkManager.h"
#include "simulation/FluidSystem.h"

#include "kinematic_body.h"
#include "keyboard_mouse.h"
#include "gamepad.h"
#include "mob.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef VOXEL_OVERWORLD_PLUGIN_PATH
#  define VOXEL_OVERWORLD_PLUGIN_PATH ""
#endif
#ifndef VOXEL_TREES_PLUGIN_PATH
#  define VOXEL_TREES_PLUGIN_PATH ""
#endif
#ifndef VOXEL_WATER_PLUGIN_PATH
#  define VOXEL_WATER_PLUGIN_PATH ""
#endif
#ifndef VOXEL_MATERIAL_AUDIO_PLUGIN_PATH
#  define VOXEL_MATERIAL_AUDIO_PLUGIN_PATH ""
#endif
#ifndef VOXEL_FLOW_PLUGIN_PATH
#  define VOXEL_FLOW_PLUGIN_PATH ""
#endif

// Reference plugins compiled into this binary (shared api() tables resolve in one
// address space — the demo-18 wiring). The host calls their *_plugin_init.
extern "C" int kinematic_body_plugin_init(PluginContext* ctx);
extern "C" int keyboard_mouse_plugin_init(PluginContext* ctx);
extern "C" int gamepad_plugin_init(PluginContext* ctx);
extern "C" int mob_plugin_init(PluginContext* ctx);

namespace {
constexpr char   kLogCat[]       = "demo20";
constexpr int    kLoadsPerFrame  = 3;
constexpr double kReachM         = 6.0;
constexpr double kEyeOffset      = 0.7;
constexpr double kPadLookSpeed   = 2.5;
constexpr float  kSafeFallSpeed  = 12.0f;
constexpr float  kFallDamageK    = 6.0f;
constexpr float  kRegenPerSec    = 6.0f;
constexpr double kStrideSeconds  = 0.42;
constexpr int    kNumMobs        = 6;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);
constexpr uint64_t kDefaultSeed  = 0xA11CE5EEDull;

// ───────────────────────── Asset synthesis ──────────────────────────────────
// The demo writes its own texture tiles and sound assets at startup (the demo-12
// "generate your own asset" pattern) so the repo carries no binaries.

uint32_t hash2(int x, int y, uint32_t s) {
    uint32_t h = s ^ (static_cast<uint32_t>(x) * 374761393u) ^ (static_cast<uint32_t>(y) * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// ── Minimal RGBA PNG writer (zlib stored blocks; decodes in stb/bimg) ─────────
uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return crc;
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    put32(out, crc32_of(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    // Raw filtered scanlines: a leading 0 filter byte per row.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + w * 4));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * w * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 4);
    }
    // zlib: 0x78 0x01, stored deflate blocks, Adler-32 trailer.
    std::vector<uint8_t> z; z.push_back(0x78); z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);  // BFINAL on last block
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(w)); put32(ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);

    std::vector<uint8_t> out = {137, 80, 78, 71, 13, 10, 26, 10};
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return f.good();
}

// Procedural 16×16 tile: base color jittered by a little per-pixel noise, with an
// optional top band (for grass_side) — enough to read as a textured surface.
void makeTile(const std::string& path, uint8_t r, uint8_t g, uint8_t b,
              int jitter, uint32_t seed, int topBandRows = 0,
              uint8_t tr = 0, uint8_t tg = 0, uint8_t tb = 0) {
    constexpr int N = 16;
    std::vector<uint8_t> px(static_cast<size_t>(N) * N * 4);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const int n = static_cast<int>(hash2(x, y, seed) % 32) - 16;
            uint8_t cr = r, cg = g, cb = b;
            if (y < topBandRows) { cr = tr; cg = tg; cb = tb; }
            auto clamp = [](int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); };
            const size_t i = (static_cast<size_t>(y) * N + x) * 4;
            px[i + 0] = clamp(cr + n * jitter / 16);
            px[i + 1] = clamp(cg + n * jitter / 16);
            px[i + 2] = clamp(cb + n * jitter / 16);
            px[i + 3] = 255;
        }
    writePng(path, N, N, px);
}

void ensureTextures(const std::string& dir) {
    std::error_code ec; std::filesystem::create_directories(dir, ec);
    auto p = [&](const char* n) { return dir + "/" + n + ".png"; };
    if (std::filesystem::exists(p("grass_top"))) return;  // generated on a prior run
    makeTile(p("grass_top"),  72, 140, 64, 10, 1);
    makeTile(p("grass_side"), 110, 84, 56, 8, 2, 4, 72, 140, 64);  // grass lip over dirt
    makeTile(p("dirt"),       110, 84, 56, 8, 3);
    makeTile(p("stone"),      120, 120, 124, 6, 4);
    makeTile(p("sand"),       214, 200, 150, 6, 5);
    makeTile(p("log_top"),    150, 110, 70, 10, 6);
    makeTile(p("log_side"),   96, 70, 44, 12, 7);
    makeTile(p("leaves"),     58, 120, 52, 14, 8);
}

// ── Minimal mono 16-bit WAV writer + a few seamless-loop synths ───────────────
bool writeWav(const std::string& path, const std::vector<int16_t>& pcm, int sampleRate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const int ch = 1, bps = 16, blockAlign = ch * bps / 8;
    const int byteRate = sampleRate * blockAlign;
    const int dataSize = static_cast<int>(pcm.size()) * blockAlign;
    auto w16 = [&](uint16_t v) { f.put(char(v & 0xff)); f.put(char((v >> 8) & 0xff)); };
    auto w32 = [&](uint32_t v) { w16(uint16_t(v & 0xffff)); w16(uint16_t((v >> 16) & 0xffff)); };
    f.write("RIFF", 4); w32(uint32_t(36 + dataSize)); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(uint16_t(ch));
    w32(uint32_t(sampleRate)); w32(uint32_t(byteRate));
    w16(uint16_t(blockAlign)); w16(uint16_t(bps));
    f.write("data", 4); w32(uint32_t(dataSize));
    f.write(reinterpret_cast<const char*>(pcm.data()),
            static_cast<std::streamsize>(pcm.size() * sizeof(int16_t)));
    return f.good();
}
int16_t toS16(double s) { return static_cast<int16_t>(std::clamp(s, -1.0, 1.0) * 32000.0); }

void ensureSounds(const std::string& dir) {
    std::error_code ec; std::filesystem::create_directories(dir, ec);
    constexpr double kPi = 3.14159265358979323846;
    const int sr = 22050;
    auto p = [&](const char* n) { return dir + "/" + n + ".wav"; };
    if (std::filesystem::exists(p("nature_bed"))) return;

    // Nature bed: a soft breeze (filtered noise) under a slow cricket pulse — a
    // 4 s seamless loop (integer cycles of the LFO).
    {
        const int frames = sr * 4;
        std::vector<int16_t> pcm(static_cast<size_t>(frames));
        double lp = 0.0; uint32_t rng = 12345;
        for (int i = 0; i < frames; ++i) {
            const double t = double(i) / sr;
            rng = rng * 1664525u + 1013904223u;
            const double white = (double(rng >> 9) / 8388608.0) - 1.0;
            lp += 0.04 * (white - lp);                       // breeze
            const double cricket = 0.12 * std::sin(2 * kPi * 2200 * t) *
                                   std::max(0.0, std::sin(2 * kPi * 4 * t));  // 4 Hz chirp pulse
            pcm[size_t(i)] = toS16(0.5 * lp + cricket);
        }
        writeWav(p("nature_bed"), pcm, sr);
    }
    // Bird: a short two-note whistle, looped over 3 s of mostly silence.
    {
        const int frames = sr * 3;
        std::vector<int16_t> pcm(static_cast<size_t>(frames), 0);
        for (int i = 0; i < sr / 3; ++i) {
            const double t = double(i) / sr;
            const double f = (i < sr / 6) ? 1800.0 : 2400.0;
            const double env = std::sin(kPi * double(i) / (sr / 3.0));
            pcm[size_t(i)] = toS16(0.3 * env * std::sin(2 * kPi * f * t));
        }
        writeWav(p("bird"), pcm, sr);
    }
    // Zombie growl: low rumble (60–90 Hz) with slow tremolo, 2 s seamless loop.
    {
        const int frames = sr * 2;
        std::vector<int16_t> pcm(static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i) {
            const double t = double(i) / sr;
            const double trem = 0.6 + 0.4 * std::sin(2 * kPi * 1.5 * t);
            const double s = 0.5 * std::sin(2 * kPi * 70 * t) +
                             0.2 * std::sin(2 * kPi * 92 * t);
            pcm[size_t(i)] = toS16(trem * s);
        }
        writeWav(p("zombie_growl"), pcm, sr);
    }
    // Zombie bite: a short noisy chomp.
    {
        const int frames = sr / 4;
        std::vector<int16_t> pcm(static_cast<size_t>(frames));
        uint32_t rng = 999;
        for (int i = 0; i < frames; ++i) {
            rng = rng * 1664525u + 1013904223u;
            const double white = (double(rng >> 9) / 8388608.0) - 1.0;
            const double env = std::exp(-12.0 * double(i) / frames);
            pcm[size_t(i)] = toS16(0.6 * env * white);
        }
        writeWav(p("zombie_bite"), pcm, sr);
    }
}

// Scan a column for the topmost solid voxel; returns its world Y, or INT_MIN.
int findSurfaceY(World& world, double x, double z, int yTop) {
    for (int y = yTop; y >= 0; --y) {
        const Voxel v = world.getVoxel(WorldCoord(x, y + 0.5, z));
        if (!v.isEmpty()) return y;
    }
    return -1000000;
}

uint8_t hudColorForPalette(uint8_t idx) {
    switch (idx) {
        case 1:  return hud::LightGray;   // stone
        case 2:  return hud::LightGreen;  // grass
        case 3:  return hud::Brown;       // dirt
        case 4:  return hud::Green;       // leaves
        case 5:  return hud::LightBlue;   // water
        case 6:  return hud::Yellow;      // sand
        case 8:  return hud::Brown;       // log
        case 11: return hud::Cyan;        // iron
        default: return hud::White;
    }
}

}  // namespace

int main(int argc, char** argv) {
    // ── World seed (the M18 subtask) ────────────────────────────────────────────
    uint64_t worldSeed = kDefaultSeed;
    if (argc > 1) {
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(argv[1], &end, 0);
        if (end && *end == '\0') worldSeed = parsed;
        else Log::warn(kLogCat, "Could not parse seed argument; using the default.");
    }
    Log::info(kLogCat, (std::string("World seed: ") + std::to_string(worldSeed)
                        + " (pass a different number as argv[1] to regenerate a new world).").c_str());

    // Resolve relative asset paths from the repo root when not launched there, so
    // both the generated assets and the optional material-audio plugin's bundled
    // WAVs load (the demo-12 working-directory convention).
#ifdef VOXEL_REPO_ROOT
    if (!std::filesystem::exists("assets/audio")) {
        std::error_code ec; std::filesystem::current_path(VOXEL_REPO_ROOT, ec);
    }
#endif
    ensureTextures("assets/textures/overworld");
    ensureSounds("assets/audio/nature");

    // ── Layer config: one terminal heightmap layer, a single vertical band ──────
    LayerConfig layerConfig = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 48
    view_distance_chunks: 3
)");
        } catch (const std::exception& e) {
            Log::error(kLogCat, (std::string("Fatal: layer config error: ") + e.what()).c_str());
            std::exit(1);
        }
    }();
    const LayerDef& terrain = layerConfig.layers().front();

    PluginManager pm;
    Engine engine;

    // ── Window + renderer + texture atlas (M15) ─────────────────────────────────
    platform::Window window(1280, 720, "VoxelEngine — M18 Mega-Demo (Overworld)");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);
    renderer.setFarClip(400.0f);
    // Distance-obscurance fog (M16): fade the far chunks into the sky.
    FogParams fog; fog.color = glm::vec3(0.62f, 0.74f, 0.86f);
    fog.near_m = 120.0f; fog.far_m = 360.0f; fog.density = 1.0f;
    renderer.setFog(fog);
    renderer.setClearColor(glm::vec3(0.62f, 0.74f, 0.86f));

    // TextureManager self-registers with the PluginManager; construct it BEFORE the
    // worldgen plugin so overworld's register_texture calls feed the atlas.
    texture::TextureManager textureManager(pm, renderer);

    // ── Plugins ─────────────────────────────────────────────────────────────────
    // Worldgen from disk; reference plugins compiled in (shared api() tables).
    auto loadOrDie = [&](const char* path, const char* name) {
        if (std::string(path).empty() || pm.loadPlugin(path) == kInvalidPluginId) {
            Log::error(kLogCat, (std::string("Fatal: could not load ") + name
                                 + " plugin from '" + path + "'.").c_str());
            std::exit(1);
        }
    };
    loadOrDie(VOXEL_OVERWORLD_PLUGIN_PATH, "overworld");  // terrain + cave/ore + materials/textures
    loadOrDie(VOXEL_TREES_PLUGIN_PATH, "trees");          // tree feature overlay
    loadOrDie(VOXEL_WATER_PLUGIN_PATH, "water");          // sea-level flood
    // material-audio is optional (break/place/footstep cues).
    const bool materialAudio = !std::string(VOXEL_MATERIAL_AUDIO_PLUGIN_PATH).empty() &&
        pm.loadPlugin(VOXEL_MATERIAL_AUDIO_PLUGIN_PATH) != kInvalidPluginId;
    if (!materialAudio)
        Log::warn(kLogCat, "material-audio plugin not loaded — break/place/footstep cues silent.");

    pm.wireInPlugin(kinematic_body_plugin_init);
    pm.wireInPlugin(keyboard_mouse_plugin_init);
    pm.wireInPlugin(gamepad_plugin_init);
    pm.wireInPlugin(mob_plugin_init);
    mob::api().set_seed(worldSeed);

    // Build the texture atlas now that overworld has registered its tiles.
    textureManager.rebuild();
    renderer.setAtlas(textureManager.atlas());
    Log::info(kLogCat, (std::string("Texture atlas: ")
                        + std::to_string(textureManager.tileCount()) + " tiles.").c_str());

    // Terrain layer generator (host overrides its user_data with &worldSeed so the
    // CLI seed drives generation — the §4 determinism knob).
    LayerGeneratorFn generator = nullptr;
    for (const auto& g : pm.layerGenerators())
        if (g.layer_name == "terrain") generator = g.fn;
    if (!generator) { Log::error(kLogCat, "Fatal: no 'terrain' generator."); return 1; }

    World world(terrain);
    engine.init(pm, world);
    engine.setRenderer(&renderer);

    // ── M14 fluid ───────────────────────────────────────────────────────────────
    // Route edits through the choke point (NetworkManager) so the flow plugin's
    // apply_edit water writes fire the engine hook; attach a FluidSystem the host
    // ticks each frame; load the mandatory `flow` responder that realizes water
    // voxels from saturated field cells. Overworld registers a spring above spawn.
    net::NetworkManager nm; nm.init(world, pm);
    sim::FluidSystem fluid(world, pm);
    engine.setFluidSystem(&fluid);
    const bool flowLoaded = !std::string(VOXEL_FLOW_PLUGIN_PATH).empty() &&
        pm.loadPlugin(VOXEL_FLOW_PLUGIN_PATH) != kInvalidPluginId;
    if (!flowLoaded)
        Log::warn(kLogCat, "flow plugin not loaded — the fluid spring will not realize water.");

    // Track chunks touched by ANY edit (player or the flow plugin's apply_edit) so
    // exactly those are re-meshed each frame.
    struct EditTracker {
        std::unordered_set<ChunkCoord, ChunkCoordHash> touched;
        int    chunkSize;
        double voxelSize;
    } tracker{ {}, world.chunkSizeVoxels(), world.voxelSizeM() };
    pm.registerEngineVoxelModifiedHook(
        [](WorldCoord pos, const Voxel*, const Voxel*, PlayerId, void* ud) {
            auto* t = static_cast<EditTracker*>(ud);
            const chunkmath::VoxelCoord v = chunkmath::worldToVoxel(pos, t->voxelSize);
            t->touched.insert(chunkmath::voxelToChunkLocal(v, t->chunkSize).chunk);
        }, &tracker);

    LODManager lod(layerConfig);
    lod.setVerticalBand(0, 0);

    // After the base layer fills a chunk, apply every registered feature generator
    // in turn (caves, ore, water, trees), threading the world seed (demo-03 pattern).
    auto applyFeatures = [&](Chunk& chunk) {
        for (const auto& f : pm.featureGenerators())
            if (f.fn)
                f.fn(chunk.origin(), world.voxelSizeM(), world.chunkSizeVoxels(),
                     chunk.data(), nullptr, 0, worldSeed, f.user_data);
    };

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;
    auto loadChunk = [&](const ChunkCoord& c) -> bool {
        if (meshes.count(c)) return false;
        Chunk* chunk = world.loadChunk(c, generator, &worldSeed);
        if (!chunk) return false;
        applyFeatures(*chunk);
        meshes.emplace(c, ChunkMesh::build(*chunk, world.voxelSizeM()));
        return true;
    };

    // ── Inventory (one hotbar slot per material) ────────────────────────────────
    struct Slot { std::string id; uint8_t palette; uint8_t color; int count; };
    std::vector<Slot> slots;
    std::unordered_map<uint8_t, size_t> paletteToSlot;
    std::unordered_map<uint8_t, std::string> paletteToName;
    for (const auto& m : pm.materials()) {
        const MaterialProperties props = pm.material(m.material_id);
        paletteToName[props.palette_index] = m.material_id;
        if (paletteToSlot.count(props.palette_index)) continue;
        paletteToSlot[props.palette_index] = slots.size();
        slots.push_back({m.material_id, props.palette_index,
                         hudColorForPalette(props.palette_index), 0});
        if (slots.size() >= 8) break;
    }
    if (slots.empty()) { Log::error(kLogCat, "Fatal: no materials registered."); return 1; }
    slots.front().count = 32;
    size_t selectedSlot = 0;

    // ── Audio ───────────────────────────────────────────────────────────────────
    audio::MiniaudioBackend backend(/*useNullDevice=*/false);
    audio::AudioManager audioManager(&backend, pm);
    pm.setAudioManager(&audioManager);
    AudioEmitterId bedEmitter = kInvalidEmitterId, birdEmitter = kInvalidEmitterId;
    if (backend.isReady()) {
        audioManager.preloadSounds();  // loads mob + material-audio registered sounds
        audio::validateAudio(pm, &backend, audio::AudioStrictPolicy::Warn);
        // Front-end-owned ambient beds (loaded straight into the backend).
        SoundParams bedP{}; bedP.volume = 0.45f; bedP.min_distance = 10.0f; bedP.max_distance = 120.0f;
        if (backend.loadSound("nature_bed", "assets/audio/nature/nature_bed.wav", bedP)) {
            EmitterParams ep{}; ep.sound = bedP; ep.loop = true;
            bedEmitter = audioManager.createEmitter("nature_bed", WorldCoord(0, 40, 0), ep);
        }
        SoundParams birdP{}; birdP.volume = 0.5f; birdP.min_distance = 6.0f; birdP.max_distance = 60.0f;
        if (backend.loadSound("bird", "assets/audio/nature/bird.wav", birdP)) {
            EmitterParams ep{}; ep.sound = birdP; ep.loop = true;
            birdEmitter = audioManager.createEmitter("bird", WorldCoord(24, 34, 18), ep);
        }
    } else {
        Log::warn(kLogCat, "Audio backend unavailable — running silently.");
    }

    // ── Input plugins (host RawSource adapters over GLFW; the demo-18 wiring) ────
    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    kbinput::RawSource kbSrc;
    kbSrc.key          = [](int kc, void* u) { return glfwGetKey((GLFWwindow*)u, kc) == GLFW_PRESS ? 1 : 0; };
    kbSrc.mouse_button = [](int b,  void* u) { return glfwGetMouseButton((GLFWwindow*)u, b) == GLFW_PRESS ? 1 : 0; };
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

    kbinput::api().bind_axis("forward", GLFW_KEY_S, GLFW_KEY_W);
    kbinput::api().bind_axis("strafe",  GLFW_KEY_A, GLFW_KEY_D);
    kbinput::api().bind_key("jump", GLFW_KEY_SPACE);
    kbinput::api().bind_mouse_button("mine",  GLFW_MOUSE_BUTTON_LEFT);
    kbinput::api().bind_mouse_button("place", GLFW_MOUSE_BUTTON_RIGHT);
    for (size_t i = 0; i < slots.size() && i < 9; ++i)
        kbinput::api().bind_key(("slot" + std::to_string(i + 1)).c_str(), GLFW_KEY_1 + int(i));
    kbinput::api().set_mouse_sensitivity(0.0022);

    gpinput::api().bind_button("jump",      GLFW_GAMEPAD_BUTTON_A);
    gpinput::api().bind_trigger("mine",     gpinput::AxisRightTrigger);
    gpinput::api().bind_trigger("place",    gpinput::AxisLeftTrigger);
    gpinput::api().bind_button("slot_next", GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
    gpinput::api().bind_button("slot_prev", GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);

    // ── Pre-stream spawn neighbourhood, then place the player on the surface ─────
    {
        ChunkCoord c0 = chunkmath::worldToChunk(WorldCoord(0.5, 24, 0.5),
                                                world.voxelSizeM(), world.chunkSizeVoxels());
        for (const ChunkCoord& c : lod.desiredChunks(c0, "terrain")) loadChunk(c);
    }
    const int surfY = std::max(findSurfaceY(world, 0.5, 0.5, 47), 18);
    kinbody::BodyDesc desc;
    desc.center     = WorldCoord(0.5, surfY + 2.0, 0.5);
    desc.eye_offset = kEyeOffset;
    const kinbody::BodyId player = kinbody::api().create_body(&desc);
    if (player == kinbody::kInvalidBody) { Log::error(kLogCat, "Fatal: no player body."); return 1; }
    const WorldCoord spawn = desc.center;

    // ── Spawn mobs at seeded surface positions around the player ────────────────
    {
        uint64_t s = worldSeed ^ 0x5A1Du;
        for (int i = 0; i < kNumMobs; ++i) {
            const double ang = voxel_rng_norm(&s) * 6.2831853;
            const double rad = 10.0 + voxel_rng_norm(&s) * 18.0;
            const double mx = 0.5 + std::cos(ang) * rad;
            const double mz = 0.5 + std::sin(ang) * rad;
            const ChunkCoord c = chunkmath::worldToChunk(WorldCoord(mx, 24, mz),
                                                         world.voxelSizeM(), world.chunkSizeVoxels());
            loadChunk(c);
            const int gy = findSurfaceY(world, mx, mz, 47);
            if (gy > 0) mob::api().spawn(WorldCoord(mx, gy + 2.0, mz));
        }
    }

    // ── Edit helpers ────────────────────────────────────────────────────────────
    auto remeshChunkOf = [&](const chunkmath::VoxelCoord& vc) {
        ChunkCoord cc = chunkmath::voxelToChunkLocal(vc, world.chunkSizeVoxels()).chunk;
        const Chunk* chunk = world.getChunk(cc);
        if (!chunk) return;
        auto it = meshes.find(cc);
        if (it != meshes.end()) { it->second.destroy(); it->second = ChunkMesh::build(*chunk, world.voxelSizeM()); }
        else meshes.emplace(cc, ChunkMesh::build(*chunk, world.voxelSizeM()));
    };
    auto editVoxel = [&](const chunkmath::VoxelCoord& vc, const Voxel& newVox) {
        const WorldCoord center = chunkmath::voxelCenter(vc, world.voxelSizeM());
        const Voxel oldVox = world.getVoxel(center);
        if (!world.setVoxel(center, newVox)) return;
        for (const auto& h : pm.voxelModifiedHooks())
            if (h.fn) h.fn(center, &oldVox, &newVox, kLocalPlayer, h.user_data);
        remeshChunkOf(vc);
    };

    // ── Player + HUD state ──────────────────────────────────────────────────────
    float pitch = -0.15f, yaw = 0.0f;
    WorldCoord camPos = spawn;
    float health = 100.0f, fallSpeed = 0.0f;
    bool prevGrounded = true, cursorCaptured = true, prevF = false;
    int activeDevice = 0;
    double strideTimer = 0.0;

    Log::info(kLogCat, "Survive the overworld. WASD move, mouse look, LMB mine, RMB place, "
                       "1-8 select, Space jump, F cursor, ESC quit.");
    auto prevTime = std::chrono::high_resolution_clock::now();

    while (!window.shouldClose()) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        window.pollEvents();
        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        const bool curF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curF && !prevF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            kbinput::api().set_source(&kbSrc);
        }
        prevF = curF;

        kbinput::api().update(dt);
        gpinput::api().update(dt);

        double padLX, padLY, padRX, padRY;
        gpinput::api().stick(gpinput::StickLeft,  &padLX, &padLY);
        gpinput::api().stick(gpinput::StickRight, &padRX, &padRY);
        double mdx, mdy;
        kbinput::api().mouse_delta(&mdx, &mdy);

        const bool padActive = gpinput::api().connected() &&
            (std::abs(padLX) + std::abs(padLY) + std::abs(padRX) + std::abs(padRY) > 0.2 ||
             gpinput::api().held("jump") || gpinput::api().held("mine") || gpinput::api().held("place"));
        const bool kbActive = (mdx != 0.0 || mdy != 0.0) ||
            kbinput::api().axis("forward") != 0.0 || kbinput::api().axis("strafe") != 0.0 ||
            kbinput::api().held("jump") || kbinput::api().held("mine") || kbinput::api().held("place");
        if (padActive) activeDevice = 1; else if (kbActive) activeDevice = 0;

        if (activeDevice == 1) {
            yaw   += static_cast<float>(padRX * kPadLookSpeed * dt);
            pitch -= static_cast<float>(padRY * kPadLookSpeed * dt);
        } else if (cursorCaptured) {
            yaw   += static_cast<float>(mdx);
            pitch -= static_cast<float>(mdy);
        }
        pitch = std::max(-1.55f, std::min(1.55f, pitch));

        double fwd = 0.0, strafe = 0.0;
        if (activeDevice == 1) { fwd = -padLY; strafe = padLX; }
        else { fwd = kbinput::api().axis("forward"); strafe = kbinput::api().axis("strafe"); }
        const double sy = std::sin(yaw), cy = std::cos(yaw);
        const glm::dvec3 fwdH(sy, 0.0, cy), rightH(cy, 0.0, -sy);
        glm::dvec3 wish = fwdH * fwd + rightH * strafe;
        const bool jump = (activeDevice == 1) ? gpinput::api().pressed("jump")
                                              : kbinput::api().pressed("jump");

        kinbody::BodyInput in;
        in.wish_x = wish.x; in.wish_y = 0.0; in.wish_z = wish.z; in.jump = jump;
        kinbody::api().set_input(player, &in);

        // Push the player AABB to the mob AI, then step all tick hooks (player body
        // + mob AI) via engine.update.
        const kinbody::BodyState* pst = kinbody::api().get_state(player);
        const WorldCoord pc = pst ? pst->center : spawn;
        mob::api().set_player(pc, kPlayerHalf.x, kPlayerHalf.y, kPlayerHalf.z);
        engine.update(dt);

        const kinbody::BodyState* st = kinbody::api().get_state(player);
        const WorldCoord playerCenter = st ? st->center : spawn;
        const bool grounded = st && st->grounded;

        // Fall damage + regen + respawn (demo-18 model).
        if (st) {
            if (!grounded) fallSpeed = std::max(fallSpeed, static_cast<float>(-st->vel_y));
            if (grounded && !prevGrounded && fallSpeed > kSafeFallSpeed)
                health = std::max(0.0f, health - (fallSpeed - kSafeFallSpeed) * kFallDamageK);
            if (grounded) fallSpeed = 0.0f;
        }
        prevGrounded = grounded;
        if (grounded && health < 100.0f) health = std::min(100.0f, health + kRegenPerSec * dt);

        // Mob melee → health.
        float dmg = 0.0f;
        if (mob::api().poll_attack(&dmg)) health = std::max(0.0f, health - dmg);

        if (health <= 0.0f || playerCenter.value.y < -8.0) {
            kinbody::api().set_position(player, spawn);
            health = 100.0f; fallSpeed = 0.0f; prevGrounded = true;
        }
        camPos = WorldCoord(playerCenter.value + glm::dvec3(0.0, kEyeOffset, 0.0));

        // Footsteps (material under the feet).
        if (grounded && glm::length(glm::dvec3(wish.x, 0, wish.z)) > 0.0) {
            strideTimer += dt;
            if (strideTimer >= kStrideSeconds) {
                strideTimer = 0.0;
                const glm::dvec3 feet = playerCenter.value - glm::dvec3(0, kPlayerHalf.y + 0.05, 0);
                const Voxel ground = world.getVoxel(WorldCoord(feet));
                if (!ground.isEmpty())
                    audioManager.playMaterialSound(AudioEvent::Footstep,
                                                   ground.material.palette_index, WorldCoord(feet));
            }
        } else strideTimer = kStrideSeconds;

        // Slot selection.
        if (activeDevice == 1) {
            if (gpinput::api().pressed("slot_next")) selectedSlot = (selectedSlot + 1) % slots.size();
            if (gpinput::api().pressed("slot_prev")) selectedSlot = (selectedSlot + slots.size() - 1) % slots.size();
        } else {
            for (size_t i = 0; i < slots.size() && i < 9; ++i)
                if (kbinput::api().pressed(("slot" + std::to_string(i + 1)).c_str())) selectedSlot = i;
        }

        // Listener.
        if (backend.isReady()) {
            const float sp = std::sin(pitch), cp = std::cos(pitch);
            glm::vec3 fdir{cp * std::sin(yaw), sp, cp * std::cos(yaw)};
            audioManager.setListener(camPos, fdir, glm::vec3(0, 1, 0));
        }

        // ── Stream chunks ───────────────────────────────────────────────────────
        ChunkCoord center = chunkmath::worldToChunk(camPos, world.voxelSizeM(), world.chunkSizeVoxels());
        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain"))
            if (loadChunk(c) && ++loaded >= kLoadsPerFrame) break;
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, "terrain") && !world.isChunkDirty(kv.first))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& c : toEvict) { meshes[c].destroy(); meshes.erase(c); world.unloadChunk(c); }

        // ── Targeting + edits ─────────────────────────────────────────────────────
        const float sp = std::sin(pitch), cp = std::cos(pitch);
        glm::dvec3 lookDir{cp * std::sin(yaw), sp, cp * std::cos(yaw)};
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);
        const Voxel target = hit.hit
            ? world.getVoxel(chunkmath::voxelCenter(hit.voxel, world.voxelSizeM())) : Voxel::empty();

        const bool mine  = (activeDevice == 1) ? gpinput::api().pressed("mine")  : kbinput::api().pressed("mine");
        const bool place = (activeDevice == 1) ? gpinput::api().pressed("place") : kbinput::api().pressed("place");

        if (hit.hit && mine && target.material.hardness >= 0.0f &&
            target.material.palette_index != 14 /* bedrock */) {
            const size_t slot = paletteToSlot.count(target.material.palette_index)
                ? paletteToSlot[target.material.palette_index] : selectedSlot;
            slots[slot].count++;
            editVoxel(hit.voxel, Voxel::empty());
        }
        if (hit.hit && place && slots[selectedSlot].count > 0) {
            const chunkmath::VoxelCoord t = hit.adjacent;
            const double vs = world.voxelSizeM();
            glm::dvec3 cmin{t.x * vs, t.y * vs, t.z * vs}, cmax = cmin + glm::dvec3(vs, vs, vs);
            glm::dvec3 pmin = playerCenter.value - kPlayerHalf, pmax = playerCenter.value + kPlayerHalf;
            const bool blocked = pmin.x < cmax.x && pmax.x > cmin.x && pmin.y < cmax.y &&
                                 pmax.y > cmin.y && pmin.z < cmax.z && pmax.z > cmin.z;
            if (!blocked) {
                Voxel v; v.material = pm.material(slots[selectedSlot].id);
                slots[selectedSlot].count--;
                editVoxel(t, v);
            }
        }

        if (backend.isReady()) audioManager.update();

        // ── M14 fluid pass: advance the field, realize water, remesh touches ──────
        fluid.tick(dt);
        for (const ChunkCoord& cc : tracker.touched) {
            const Chunk* ch = world.getChunk(cc);
            if (!ch) continue;
            auto it = meshes.find(cc);
            if (it != meshes.end()) { it->second.destroy(); it->second = ChunkMesh::build(*ch, world.voxelSizeM()); }
            else meshes.emplace(cc, ChunkMesh::build(*ch, world.voxelSizeM()));
        }
        tracker.touched.clear();

        // ── Render ───────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        for (const auto& kv : meshes) {
            const Chunk* chunk = world.getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin());
        }

        // Mobs: a stacked-voxel body, tinted by AI state (the engine has no entity
        // system, so the front-end draws the body itself).
        const uint32_t mobN = mob::api().mob_count();
        int chasing = 0;
        for (uint32_t i = 0; i < mobN; ++i) {
            const mob::MobState* m = mob::api().get_mob(i);
            if (!m || m->state == mob::State::Dead) continue;
            if (m->state != mob::State::Wander) ++chasing;
            uint32_t body = 0xff3f8f3f, head = 0xff2f6f2f;  // green idle
            if (m->state == mob::State::Chase)  { body = 0xff2f7fcf; head = 0xff1f5faf; }  // orange (ABGR)
            if (m->state == mob::State::Attack) { body = 0xff3030d0; head = 0xff2020a0; }  // red
            renderer.drawVoxel(WorldCoord(m->center.value + glm::dvec3(0, -0.4, 0)), body);
            renderer.drawVoxel(WorldCoord(m->center.value + glm::dvec3(0,  0.7, 0)), head);
        }

        if (hit.hit)
            renderer.drawVoxelHighlight(chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()),
                                        static_cast<float>(world.voxelSizeM()), 0xff00ffff, -1.0f);

        // ── HUD: health bar, hotbar, status line ──────────────────────────────────
        renderer.hudClear();
        const int filled = static_cast<int>(std::round(health / 100.0f * 20));
        const uint8_t hpColor = health > 30.0f ? hud::LightGreen : hud::LightRed;
        renderer.hudText(1, 1, hud::attr(hud::White), "HP");
        renderer.hudFill(4, 1, filled, 1, hud::attr(hud::Black, hpColor));
        renderer.hudFill(4 + filled, 1, 20 - filled, 1, hud::attr(hud::Black, hud::DarkGray));
        for (size_t i = 0; i < slots.size(); ++i) {
            const int col = 1 + static_cast<int>(i) * 6;
            const uint8_t a = (i == selectedSlot) ? hud::attr(hud::Black, hud::White)
                                                  : hud::attr(slots[i].color, hud::Black);
            char cell[8]; std::snprintf(cell, sizeof(cell), "%d:%d", int(i + 1), slots[i].count);
            renderer.hudText(col, 3, a, cell);
        }
        char status[128];
        std::snprintf(status, sizeof(status),
                      "Seed %llu | %s | XYZ %.0f %.0f %.0f | mobs %u (%d hostile) | %s",
                      static_cast<unsigned long long>(worldSeed),
                      slots[selectedSlot].id.c_str(),
                      camPos.value.x, camPos.value.y, camPos.value.z,
                      mobN, chasing, activeDevice == 1 ? "gamepad" : "kbd/mouse");
        renderer.hudText(1, 5, hud::attr(hud::Yellow), status);

        renderer.render();
    }

    if (bedEmitter  != kInvalidEmitterId) audioManager.stopEmitter(bedEmitter);
    if (birdEmitter != kInvalidEmitterId) audioManager.stopEmitter(birdEmitter);
    pm.setAudioManager(nullptr);
    for (auto& kv : meshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

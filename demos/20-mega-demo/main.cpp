// M18.5 demo — the Mega-Demo ("Overworld"): a Minecraft-lite survival slice
// that exercises as much of the engine as one coherent world can.
//
// M18.5 revision: the world is now a COMPOSITE heightmap — a coarse "blocks"
// layer (4 m macros) sits above the 1 m "terrain" terminal layer. The blocks
// layer has no block voxels (every macro is "decomposed"); the engine's
// PropagationSystem aggregates the terrain children's structural_strength into
// each macro, and the support-potential flood detects unsupported spans.
// Mining under an overhang triggers a real cave-in via the `crumble` response
// plugin (the M13 headline the M18 demo previously disclaimed). An immutable
// "bedrock" layer anchors the bottom so the cascade stops at the world floor.
//
// New in M18.5: the player can ATTACK mobs (Q or LMB-in-air), dealing 20
// damage per hit. Dead mobs stop chasing. Fight back instead of just running.
//
// Controls: WASD/stick move, mouse/stick look, Space/A jump, LMB/RT mine,
// RMB/LT place, Q/X attack mob, 1-8 select material, F toggles cursor, ESC quits.

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
#include "simulation/PhysicsSystem.h"

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
#ifndef VOXEL_CRUMBLE_PLUGIN_PATH
#  define VOXEL_CRUMBLE_PLUGIN_PATH ""
#endif

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
constexpr float  kAttackDamage   = 20.0f;
constexpr double kAttackCooldown = 0.4;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);
constexpr uint64_t kDefaultSeed  = 0xA11CE5EEDull;

// Composite layer geometry.
constexpr double kBlocksVoxelSizeM = 4.0;  // macro voxel edge length
constexpr double kTerrainVoxelSizeM = 1.0;
constexpr int    kRatio = 4;  // terrain voxels per blocks macro edge (4 m / 1 m)
// Coarse immutable anchor. A single 16 m voxel layer at the world floor is the
// cheapest way to give the PropagationSystem a real immutable boundary: a few
// large voxels blanket the whole streamed area, versus a dense fine grid. 16 m
// keeps the ratio over the 4 m blocks layer a whole integer (4:1).
constexpr double kBedrockVoxelSizeM = 16.0;

// ───────────────────────── Asset synthesis ──────────────────────────────────

uint32_t hash2(int x, int y, uint32_t s) {
    uint32_t h = s ^ (static_cast<uint32_t>(x) * 374761393u) ^ (static_cast<uint32_t>(y) * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

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
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + w * 4));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * w * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 4);
    }
    std::vector<uint8_t> z; z.push_back(0x78); z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
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
    if (std::filesystem::exists(p("grass_top"))) return;
    makeTile(p("grass_top"),  72, 140, 64, 10, 1);
    makeTile(p("grass_side"), 110, 84, 56, 8, 2, 4, 72, 140, 64);
    makeTile(p("dirt"),       110, 84, 56, 8, 3);
    makeTile(p("stone"),      120, 120, 124, 6, 4);
    makeTile(p("sand"),       214, 200, 150, 6, 5);
    makeTile(p("log_top"),    150, 110, 70, 10, 6);
    makeTile(p("log_side"),   96, 70, 44, 12, 7);
    makeTile(p("leaves"),     58, 120, 52, 14, 8);
}

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

    {
        const int frames = sr * 4;
        std::vector<int16_t> pcm(static_cast<size_t>(frames));
        double lp = 0.0; uint32_t rng = 12345;
        for (int i = 0; i < frames; ++i) {
            const double t = double(i) / sr;
            rng = rng * 1664525u + 1013904223u;
            const double white = (double(rng >> 9) / 8388608.0) - 1.0;
            lp += 0.04 * (white - lp);
            const double cricket = 0.12 * std::sin(2 * kPi * 2200 * t) *
                                   std::max(0.0, std::sin(2 * kPi * 4 * t));
            pcm[size_t(i)] = toS16(0.5 * lp + cricket);
        }
        writeWav(p("nature_bed"), pcm, sr);
    }
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

int findSurfaceY(World& world, double x, double z, int yTop) {
    for (int y = yTop; y >= 0; --y) {
        const Voxel v = world.getVoxel(WorldCoord(x, y + 0.5, z));
        if (!v.isEmpty()) return y;
    }
    return -1000000;
}

uint8_t hudColorForPalette(uint8_t idx) {
    switch (idx) {
        case 1:  return hud::LightGray;
        case 2:  return hud::LightGreen;
        case 3:  return hud::Brown;
        case 4:  return hud::Green;
        case 5:  return hud::LightBlue;
        case 6:  return hud::Yellow;
        case 8:  return hud::Brown;
        case 11: return hud::Cyan;
        default: return hud::White;
    }
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t worldSeed = kDefaultSeed;
    if (argc > 1) {
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(argv[1], &end, 0);
        if (end && *end == '\0') worldSeed = parsed;
        else Log::warn(kLogCat, "Could not parse seed argument; using the default.");
    }
    Log::info(kLogCat, (std::string("World seed: ") + std::to_string(worldSeed)
                        + " (pass a different number as argv[1] to regenerate a new world).").c_str());

#ifdef VOXEL_REPO_ROOT
    if (!std::filesystem::exists("assets/audio")) {
        std::error_code ec; std::filesystem::current_path(VOXEL_REPO_ROOT, ec);
    }
#endif
    ensureTextures("assets/textures/overworld");
    ensureSounds("assets/audio/nature");

    // ── Layer config: immutable "bedrock" → composite "blocks" → terminal "terrain"
    // The layer stack is coarsest-first (LayerConfig requires strictly descending
    // voxel sizes with whole-integer ratios ≥ 2). The "bedrock" immutable layer
    // (16 m macros) is the coarse root: a single voxel layer at the world floor
    // that the PropagationSystem treats as an infinite-effective anchor, so a
    // cave-in cascade stops dead at the floor. Coarse on purpose — a handful of
    // 16 m voxels blanket the streamed area for almost no memory, versus a dense
    // fine grid (a fine immutable floor would allocate a near-empty chunk per
    // terrain chunk). The "blocks" composite layer (4 m macros, ratio 4:1 over
    // 1 m terrain) gives the PropagationSystem a structural level to aggregate and
    // flood. The terrain layer is marked interactive so the single-layer World API
    // (getVoxel/setVoxel) targets it.
    LayerConfig layerConfig = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: bedrock
    voxel_size_m: 16.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 3
  - name: blocks
    voxel_size_m: 4.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 12
    view_distance_chunks: 3
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 48
    view_distance_chunks: 3
    interactive: true
)");
        } catch (const std::exception& e) {
            Log::error(kLogCat, (std::string("Fatal: layer config error: ") + e.what()).c_str());
            std::exit(1);
        }
    }();

    PluginManager pm;
    Engine engine;

    platform::Window window(1280, 720, "VoxelEngine — M18.5 Mega-Demo (Overworld)");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);
    renderer.setFarClip(400.0f);
    FogParams fog; fog.color = glm::vec3(0.62f, 0.74f, 0.86f);
    fog.near_m = 120.0f; fog.far_m = 360.0f; fog.density = 1.0f;
    renderer.setFog(fog);
    renderer.setClearColor(glm::vec3(0.62f, 0.74f, 0.86f));

    texture::TextureManager textureManager(pm, renderer);

    // ── Plugins ─────────────────────────────────────────────────────────────────
    auto loadOrDie = [&](const char* path, const char* name) {
        if (std::string(path).empty() || pm.loadPlugin(path) == kInvalidPluginId) {
            Log::error(kLogCat, (std::string("Fatal: could not load ") + name
                                 + " plugin from '" + path + "'.").c_str());
            std::exit(1);
        }
    };
    loadOrDie(VOXEL_OVERWORLD_PLUGIN_PATH, "overworld");
    loadOrDie(VOXEL_TREES_PLUGIN_PATH, "trees");
    loadOrDie(VOXEL_WATER_PLUGIN_PATH, "water");
    const bool materialAudio = !std::string(VOXEL_MATERIAL_AUDIO_PLUGIN_PATH).empty() &&
        pm.loadPlugin(VOXEL_MATERIAL_AUDIO_PLUGIN_PATH) != kInvalidPluginId;
    if (!materialAudio)
        Log::warn(kLogCat, "material-audio plugin not loaded — break/place/footstep cues silent.");

    // M13 structural-response plugin.
    const bool crumbleLoaded = !std::string(VOXEL_CRUMBLE_PLUGIN_PATH).empty() &&
        pm.loadPlugin(VOXEL_CRUMBLE_PLUGIN_PATH) != kInvalidPluginId;
    if (!crumbleLoaded)
        Log::warn(kLogCat, "crumble plugin not loaded — mining will not trigger cave-ins.");

    pm.wireInPlugin(kinematic_body_plugin_init);
    pm.wireInPlugin(keyboard_mouse_plugin_init);
    pm.wireInPlugin(gamepad_plugin_init);
    pm.wireInPlugin(mob_plugin_init);
    mob::api().set_seed(worldSeed);

    textureManager.rebuild();
    renderer.setAtlas(textureManager.atlas());
    Log::info(kLogCat, (std::string("Texture atlas: ")
                        + std::to_string(textureManager.tileCount()) + " tiles.").c_str());

    // Terrain layer generator.
    LayerGeneratorFn generator = nullptr;
    for (const auto& g : pm.layerGenerators())
        if (g.layer_name == "terrain") generator = g.fn;
    if (!generator) { Log::error(kLogCat, "Fatal: no 'terrain' generator."); return 1; }

    World world(layerConfig);
    Layer* blocksLayer  = world.layer("blocks");
    Layer* terrainLayer = world.layer("terrain");
    Layer* bedrockLayer = world.layer("bedrock");
    if (!blocksLayer || !terrainLayer || !bedrockLayer) {
        Log::error(kLogCat, "Fatal: expected blocks/terrain/bedrock layers."); return 1;
    }

    engine.init(pm, world);
    engine.setRenderer(&renderer);

    // ── M14 fluid ───────────────────────────────────────────────────────────────
    net::NetworkManager nm; nm.init(world, pm);
    sim::FluidSystem fluid(world, pm);
    engine.setFluidSystem(&fluid);
    const bool flowLoaded = !std::string(VOXEL_FLOW_PLUGIN_PATH).empty() &&
        pm.loadPlugin(VOXEL_FLOW_PLUGIN_PATH) != kInvalidPluginId;
    if (!flowLoaded)
        Log::warn(kLogCat, "flow plugin not loaded — the fluid spring will not realize water.");

    // ── M13 structural collapse ─────────────────────────────────────────────────
    // Constructed after init so its on_voxel_modified hook catches every edit.
    sim::PhysicsSystem physics(world, pm);

    struct EditTracker {
        std::unordered_set<ChunkCoord, ChunkCoordHash> touched;
        int    chunkSize;
        double voxelSize;
    } tracker{ {}, terrainLayer->chunkSizeVoxels(), terrainLayer->voxelSizeM() };
    pm.registerEngineVoxelModifiedHook(
        [](WorldCoord pos, const Voxel*, const Voxel*, PlayerId, void* ud) {
            auto* t = static_cast<EditTracker*>(ud);
            const chunkmath::VoxelCoord v = chunkmath::worldToVoxel(pos, t->voxelSize);
            t->touched.insert(chunkmath::voxelToChunkLocal(v, t->chunkSize).chunk);
        }, &tracker);

    LODManager lod(layerConfig);
    lod.setVerticalBand(0, 0);

    // Feature overlays applied after the base generator fills a terrain chunk.
    auto applyFeatures = [&](Chunk& chunk) {
        for (const auto& f : pm.featureGenerators())
            if (f.fn)
                f.fn(chunk.origin(), terrainLayer->voxelSizeM(),
                     terrainLayer->chunkSizeVoxels(),
                     chunk.data(), nullptr, 0, worldSeed, f.user_data);
    };

    // Bedrock generator: fill the single bottom voxel layer (the slab whose world
    // Y spans [0, 16 m)) with indestructible bedrock; everything above is empty.
    // One 16 m voxel layer is enough — the PropagationSystem only needs a
    // non-empty immutable voxel under a structure to anchor it.
    auto bedrockGenerator = [](WorldCoord origin, int n, Voxel* out, void*) {
        MaterialProperties bed;
        bed.density = 4000.0f; bed.structural_strength = 1.0f;
        bed.hardness = -1.0f; bed.palette_index = 14;
        for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const double wy = origin.value.y + (y + 0.5) * kBedrockVoxelSizeM;
                    Voxel& v = out[x + n * (y + n * z)];
                    v = (wy >= 0.0 && wy < kBedrockVoxelSizeM) ? Voxel{bed} : Voxel::empty();
                }
    };

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    // Ensure the blocks-layer and bedrock-layer chunks overlapping a terrain chunk
    // are loaded. The blocks chunks are loaded EMPTY (no block voxels — every macro
    // is "decomposed", so the PropagationSystem aggregates the terrain children
    // directly). The bedrock chunks carry one 16 m slab at the world floor so they
    // serve as structural anchors (the cascade stops at immutable voxels).
    std::unordered_set<ChunkCoord, ChunkCoordHash> loadedBlocksChunks;
    std::unordered_set<ChunkCoord, ChunkCoordHash> loadedBedrockChunks;
    auto ensureCompositeChunks = [&](const ChunkCoord& terrainCC) {
        // Compute which blocks-layer chunk(s) overlap this terrain chunk.
        const double tvs = terrainLayer->voxelSizeM();
        const int tcs = terrainLayer->chunkSizeVoxels();
        const double bvs = blocksLayer->voxelSizeM();
        const int bcs = blocksLayer->chunkSizeVoxels();

        // Terrain chunk covers world X in [terrainCC.x * tcs * tvs, (terrainCC.x+1) * tcs * tvs).
        // A blocks chunk covers [blocksCC.x * bcs * bvs, (blocksCC.x+1) * bcs * bvs).
        const double tx0 = terrainCC.x * tcs * tvs;
        const double ty0 = terrainCC.y * tcs * tvs;
        const double tz0 = terrainCC.z * tcs * tvs;
        const double tx1 = tx0 + tcs * tvs;
        const double ty1 = ty0 + tcs * tvs;
        const double tz1 = tz0 + tcs * tvs;
        const double bChunkSpan = bcs * bvs;

        const int bx0 = static_cast<int>(std::floor(tx0 / bChunkSpan));
        const int by0 = static_cast<int>(std::floor(ty0 / bChunkSpan));
        const int bz0 = static_cast<int>(std::floor(tz0 / bChunkSpan));
        const int bx1 = static_cast<int>(std::floor((tx1 - 0.001) / bChunkSpan));
        const int by1 = static_cast<int>(std::floor((ty1 - 0.001) / bChunkSpan));
        const int bz1 = static_cast<int>(std::floor((tz1 - 0.001) / bChunkSpan));

        for (int bz = bz0; bz <= bz1; ++bz)
            for (int by = by0; by <= by1; ++by)
                for (int bx = bx0; bx <= bx1; ++bx) {
                    ChunkCoord bcc{bx, by, bz};
                    if (loadedBlocksChunks.insert(bcc).second)
                        blocksLayer->loadChunk(bcc, nullptr);
                }

        // Bedrock chunk(s) overlapping this terrain chunk's XZ footprint at the
        // floor. Bedrock is coarse (16 m) on its own chunk grid, so one bedrock
        // chunk usually blankets many terrain chunks; the floor slab lives in the
        // y=0 bedrock chunk (worldY [0, 16) ⊂ chunk [0, rcs·rvs)).
        const double rvs = bedrockLayer->voxelSizeM();
        const int    rcs = bedrockLayer->chunkSizeVoxels();
        const double rChunkSpan = rcs * rvs;
        const int rx0 = static_cast<int>(std::floor(tx0 / rChunkSpan));
        const int rz0 = static_cast<int>(std::floor(tz0 / rChunkSpan));
        const int rx1 = static_cast<int>(std::floor((tx1 - 0.001) / rChunkSpan));
        const int rz1 = static_cast<int>(std::floor((tz1 - 0.001) / rChunkSpan));
        for (int rz = rz0; rz <= rz1; ++rz)
            for (int rx = rx0; rx <= rx1; ++rx) {
                ChunkCoord rcc{rx, 0, rz};
                if (loadedBedrockChunks.insert(rcc).second)
                    bedrockLayer->loadChunk(rcc, bedrockGenerator, nullptr);
            }
    };

    auto loadChunk = [&](const ChunkCoord& c) -> bool {
        if (meshes.count(c)) return false;
        Chunk* chunk = terrainLayer->loadChunk(c, generator, &worldSeed);
        if (!chunk) return false;
        applyFeatures(*chunk);
        ensureCompositeChunks(c);
        meshes.emplace(c, ChunkMesh::build(*chunk, terrainLayer->voxelSizeM()));
        return true;
    };

    // ── Inventory ──────────────────────────────────────────────────────────────
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
        audioManager.preloadSounds();
        audio::validateAudio(pm, &backend, audio::AudioStrictPolicy::Warn);
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

    // ── Input plugins ──────────────────────────────────────────────────────────
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
    kbinput::api().bind_key("attack", GLFW_KEY_Q);
    for (size_t i = 0; i < slots.size() && i < 9; ++i)
        kbinput::api().bind_key(("slot" + std::to_string(i + 1)).c_str(), GLFW_KEY_1 + int(i));
    kbinput::api().set_mouse_sensitivity(0.0022);

    gpinput::api().bind_button("jump",      GLFW_GAMEPAD_BUTTON_A);
    gpinput::api().bind_trigger("mine",     gpinput::AxisRightTrigger);
    gpinput::api().bind_trigger("place",    gpinput::AxisLeftTrigger);
    gpinput::api().bind_button("attack",    GLFW_GAMEPAD_BUTTON_X);
    gpinput::api().bind_button("slot_next", GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
    gpinput::api().bind_button("slot_prev", GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);

    // ── Pre-stream spawn neighbourhood ──────────────────────────────────────────
    {
        ChunkCoord c0 = chunkmath::worldToChunk(WorldCoord(0.5, 24, 0.5),
                                                terrainLayer->voxelSizeM(),
                                                terrainLayer->chunkSizeVoxels());
        for (const ChunkCoord& c : lod.desiredChunks(c0, "terrain")) loadChunk(c);
    }
    const int surfY = std::max(findSurfaceY(world, 0.5, 0.5, 47), 18);
    kinbody::BodyDesc desc;
    desc.center     = WorldCoord(0.5, surfY + 2.0, 0.5);
    desc.eye_offset = kEyeOffset;
    const kinbody::BodyId player = kinbody::api().create_body(&desc);
    if (player == kinbody::kInvalidBody) { Log::error(kLogCat, "Fatal: no player body."); return 1; }
    const WorldCoord spawn = desc.center;

    // ── Spawn mobs ──────────────────────────────────────────────────────────────
    {
        uint64_t s = worldSeed ^ 0x5A1Du;
        for (int i = 0; i < kNumMobs; ++i) {
            const double ang = voxel_rng_norm(&s) * 6.2831853;
            const double rad = 10.0 + voxel_rng_norm(&s) * 18.0;
            const double mx = 0.5 + std::cos(ang) * rad;
            const double mz = 0.5 + std::sin(ang) * rad;
            const ChunkCoord c = chunkmath::worldToChunk(WorldCoord(mx, 24, mz),
                                                         terrainLayer->voxelSizeM(),
                                                         terrainLayer->chunkSizeVoxels());
            loadChunk(c);
            const int gy = findSurfaceY(world, mx, mz, 47);
            if (gy > 0) mob::api().spawn(WorldCoord(mx, gy + 2.0, mz));
        }
    }

    // ── Edit helpers ────────────────────────────────────────────────────────────
    // Route edits through NetworkManager so PhysicsSystem's on_voxel_modified
    // hook observes them and fires structural events.
    auto editVoxel = [&](const chunkmath::VoxelCoord& vc, const Voxel& newVox) {
        const WorldCoord center = chunkmath::voxelCenter(vc, terrainLayer->voxelSizeM());
        nm.applyEdit(kLocalPlayer, center, newVox);
        // Remesh.
        ChunkCoord cc = chunkmath::voxelToChunkLocal(vc, terrainLayer->chunkSizeVoxels()).chunk;
        const Chunk* chunk = terrainLayer->getChunk(cc);
        if (!chunk) return;
        auto it = meshes.find(cc);
        if (it != meshes.end()) { it->second.destroy(); it->second = ChunkMesh::build(*chunk, terrainLayer->voxelSizeM()); }
        else meshes.emplace(cc, ChunkMesh::build(*chunk, terrainLayer->voxelSizeM()));
    };

    // ── Player + HUD state ──────────────────────────────────────────────────────
    float pitch = -0.15f, yaw = 0.0f;
    WorldCoord camPos = spawn;
    float health = 100.0f, fallSpeed = 0.0f;
    bool prevGrounded = true, cursorCaptured = true, prevF = false;
    int activeDevice = 0;
    double strideTimer = 0.0;
    double attackTimer = 0.0;
    int kills = 0;

    Log::info(kLogCat, "Survive the overworld. WASD move, mouse look, LMB mine, RMB place, "
                       "Q attack mob, 1-8 select, Space jump, F cursor, ESC quit.");
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

        const kinbody::BodyState* pst = kinbody::api().get_state(player);
        const WorldCoord pc = pst ? pst->center : spawn;
        mob::api().set_player(pc, kPlayerHalf.x, kPlayerHalf.y, kPlayerHalf.z);
        engine.update(dt);

        const kinbody::BodyState* st = kinbody::api().get_state(player);
        const WorldCoord playerCenter = st ? st->center : spawn;
        const bool grounded = st && st->grounded;

        // Fall damage + regen + respawn.
        if (st) {
            if (!grounded) fallSpeed = std::max(fallSpeed, static_cast<float>(-st->vel_y));
            if (grounded && !prevGrounded && fallSpeed > kSafeFallSpeed)
                health = std::max(0.0f, health - (fallSpeed - kSafeFallSpeed) * kFallDamageK);
            if (grounded) fallSpeed = 0.0f;
        }
        prevGrounded = grounded;
        if (grounded && health < 100.0f) health = std::min(100.0f, health + kRegenPerSec * dt);

        // Mob melee → player health.
        float dmg = 0.0f;
        if (mob::api().poll_attack(&dmg)) health = std::max(0.0f, health - dmg);

        if (health <= 0.0f || playerCenter.value.y < -8.0) {
            kinbody::api().set_position(player, spawn);
            health = 100.0f; fallSpeed = 0.0f; prevGrounded = true;
        }
        camPos = WorldCoord(playerCenter.value + glm::dvec3(0.0, kEyeOffset, 0.0));

        // Footsteps.
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
        ChunkCoord center = chunkmath::worldToChunk(camPos, terrainLayer->voxelSizeM(),
                                                     terrainLayer->chunkSizeVoxels());
        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain"))
            if (loadChunk(c) && ++loaded >= kLoadsPerFrame) break;
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, "terrain") && !terrainLayer->isChunkDirty(kv.first))
                toEvict.push_back(kv.first);
        for (const ChunkCoord& c : toEvict) {
            meshes[c].destroy(); meshes.erase(c);
            terrainLayer->unloadChunk(c);
        }

        // ── Targeting + edits + combat ───────────────────────────────────────────
        const float sp = std::sin(pitch), cp = std::cos(pitch);
        glm::dvec3 lookDir{cp * std::sin(yaw), sp, cp * std::cos(yaw)};
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);
        const Voxel target = hit.hit
            ? world.getVoxel(chunkmath::voxelCenter(hit.voxel, terrainLayer->voxelSizeM())) : Voxel::empty();

        const bool mine  = (activeDevice == 1) ? gpinput::api().pressed("mine")  : kbinput::api().pressed("mine");
        const bool place = (activeDevice == 1) ? gpinput::api().pressed("place") : kbinput::api().pressed("place");
        const bool attack = (activeDevice == 1) ? gpinput::api().pressed("attack") : kbinput::api().pressed("attack");

        if (hit.hit && mine && target.material.hardness >= 0.0f &&
            target.material.palette_index != 14 /* bedrock */) {
            const size_t slot = paletteToSlot.count(target.material.palette_index)
                ? paletteToSlot[target.material.palette_index] : selectedSlot;
            slots[slot].count++;
            editVoxel(hit.voxel, Voxel::empty());
        }
        if (hit.hit && place && slots[selectedSlot].count > 0) {
            const chunkmath::VoxelCoord t = hit.adjacent;
            const double vs = terrainLayer->voxelSizeM();
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

        // Player attack: Q (keyboard) or X (gamepad), or LMB when not targeting a
        // block. Deals kAttackDamage to the nearest mob within reach.
        attackTimer -= dt;
        const bool wantAttack = attack || (mine && !hit.hit);
        if (wantAttack && attackTimer <= 0.0) {
            if (mob::api().attack_nearest &&
                mob::api().attack_nearest(camPos, kReachM, kAttackDamage)) {
                attackTimer = kAttackCooldown;
                int alive = 0;
                for (uint32_t i = 0; i < mob::api().mob_count(); ++i) {
                    const mob::MobState* m = mob::api().get_mob(i);
                    if (m && m->state != mob::State::Dead) ++alive;
                }
                kills = static_cast<int>(mob::api().mob_count()) - alive;
            }
        }

        if (backend.isReady()) audioManager.update();

        // ── M14 fluid pass ──────────────────────────────────────────────────────
        fluid.tick(dt);

        // ── M13 structural pass ─────────────────────────────────────────────────
        physics.tick();

        // Remesh touched chunks (from player edits, crumble, fluid).
        for (const ChunkCoord& cc : tracker.touched) {
            const Chunk* ch = terrainLayer->getChunk(cc);
            if (!ch) continue;
            auto it = meshes.find(cc);
            if (it != meshes.end()) { it->second.destroy(); it->second = ChunkMesh::build(*ch, terrainLayer->voxelSizeM()); }
            else meshes.emplace(cc, ChunkMesh::build(*ch, terrainLayer->voxelSizeM()));
        }
        tracker.touched.clear();

        // ── Render ───────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        for (const auto& kv : meshes) {
            const Chunk* ch = terrainLayer->getChunk(kv.first);
            if (ch) renderer.renderChunk(kv.second, ch->origin());
        }

        // Mobs.
        const uint32_t mobN = mob::api().mob_count();
        int chasing = 0, aliveCount = 0;
        for (uint32_t i = 0; i < mobN; ++i) {
            const mob::MobState* m = mob::api().get_mob(i);
            if (!m || m->state == mob::State::Dead) continue;
            ++aliveCount;
            if (m->state != mob::State::Wander) ++chasing;
            uint32_t body = 0xff3f8f3f, head = 0xff2f6f2f;
            if (m->state == mob::State::Chase)  { body = 0xff2f7fcf; head = 0xff1f5faf; }
            if (m->state == mob::State::Attack) { body = 0xff3030d0; head = 0xff2020a0; }
            renderer.drawVoxel(WorldCoord(m->center.value + glm::dvec3(0, -0.4, 0)), body);
            renderer.drawVoxel(WorldCoord(m->center.value + glm::dvec3(0,  0.7, 0)), head);
        }

        if (hit.hit)
            renderer.drawVoxelHighlight(chunkmath::voxelCenter(hit.voxel, terrainLayer->voxelSizeM()),
                                        static_cast<float>(terrainLayer->voxelSizeM()), 0xff00ffff, -1.0f);

        // ── HUD ──────────────────────────────────────────────────────────────────
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
        char status[160];
        std::snprintf(status, sizeof(status),
                      "Seed %llu | %s | XYZ %.0f %.0f %.0f | mobs %d/%u (%d hostile) kills %d | %s%s",
                      static_cast<unsigned long long>(worldSeed),
                      slots[selectedSlot].id.c_str(),
                      camPos.value.x, camPos.value.y, camPos.value.z,
                      aliveCount, mobN, chasing, kills,
                      activeDevice == 1 ? "gamepad" : "kbd/mouse",
                      crumbleLoaded ? " | M13" : "");
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

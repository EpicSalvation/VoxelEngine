// M7b demo — arena platformer (all groups: world generation, decomposition,
// platformer mechanics, and game objective).
//
// A five-layer walled arena exercises the full M1–M7 feature set at once:
//
//   "foundation"  500 m immutable — solid stone floor covering the 500×500 m arena.
//   "ramparts"     20 m immutable — 40 m-thick perimeter walls, 100 m tall.
//   "terraces"     10 m composite — platform blocks that decompose into the detail
//                  layer as the player approaches (single-step, M6 pattern).
//   "props"         2 m immutable — decorative columns at eight arena positions.
//   "detail"        1 m terminal  — fine walkable surface revealed by decomposition.
//
// A stone starter staircase (authored in the arena plugin's terraces/detail
// generators) climbs from the floor up to the south edge of the central start
// pad, giving the floor-spawned walk-mode player a route onto the platform
// network — walk-mode collision has no step-up, so jump up each 1 m riser.
//
// Layer ratio chain: 25:1, 2:1, 5:1, 2:1 — all validated at startup.
//
// Game objective (Group 4 / M7):
//   - Four key totems and a goal totem are imported .vox models placed at their
//     world anchors above each non-goal platform (Engine::importVox, M7 color
//     round-trip).  Keys render with their authored gold palette colors.
//   - Walk through a key's 2 m trigger volume to collect it; a collected key's
//     voxels are cleared from the detail layer.  Reach the goal totem with all
//     four keys collected to log victory.
//   - Fall below the arena floor or touch a lava voxel to respawn at the start.
//   - Press P to load/unload the hazards plugin: lava pools appear on the top
//     surface of each platform and vanish when unloaded (M4 live-toggle pattern).
//   - Press E to export the detail layer to arena-export.vox via Engine::exportVox;
//     the 500-voxel-wide region exercises auto-chunking (>256/axis) and the
//     lossy-property warning path.
//
// Controls: WASD move, mouse look, Space/Shift fly up/down (or jump in walk mode),
// G = walk (gravity + cross-layer AABB collision), F = cursor, left mouse = break,
// right mouse = place, 1–9 = material, P = toggle lava hazards, E = export, ESC quits.
//
// Run from the build directory:
//   ./build/07-arena-platformer
// Fly toward the stone platforms to see them decompose into grass-topped 1 m detail.
// Walk into a gold key stake to collect it, then reach the goal totem to win.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "io/ChunkPersistence.h"
#include "io/VoxExporter.h"
#include "io/VoxImporter.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionWorker.h"
#include "world/MacroVoxel.h"
#include "world/VoxelCollision.h"
#include "world/VoxelRaycast.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef VOXEL_ARENA_PLUGIN_PATH
#  define VOXEL_ARENA_PLUGIN_PATH ""
#endif
#ifndef VOXEL_HAZARDS_PLUGIN_PATH
#  define VOXEL_HAZARDS_PLUGIN_PATH ""
#endif
#ifndef DEMO_ASSET_DIR
#  define DEMO_ASSET_DIR ""
#endif

namespace {

// ── Streaming / decomposition budgets ───────────────────────────────────────
constexpr int    kStreamPerFrame     =  2;     // coarse/immutable chunks meshed per frame
constexpr int    kDecomposePerFrame  = 32;     // terrace macro voxels enqueued per frame
constexpr double kDecomposeRadiusM   = 80.0;   // decompose terraces within this radius
// Decomposed detail is released when it drifts past this radius so the resident
// detail-mesh count stays below the bgfx static-buffer ceiling (4096 each).
constexpr double kDetailKeepRadiusM  = 100.0;

// ── Camera / player constants ────────────────────────────────────────────────
constexpr float  kFlySpeed   = 50.0f;
constexpr float  kMouseSens  = 0.002f;
constexpr double kWalkSpeed  =  8.0;
constexpr double kGravity    = 25.0;
constexpr double kJumpSpeed  =  9.0;
constexpr double kEyeOffset  =  0.7;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

// ── Build / edit constants ────────────────────────────────────────────────────
constexpr double kReachM = 8.0;  // how far the player can target a detail voxel

// ── Palette index constants (must match arena plugin) ────────────────────────
constexpr uint8_t kLavaIdx = 9;

// ── Spawn / respawn position ─────────────────────────────────────────────────
const glm::dvec3 kSpawnPos(250.0, 5.0, 80.0);

// ── Key and goal totem world anchors ─────────────────────────────────────────
// Each key is a 1×2×1 gold stake placed just above the top surface of its
// platform (y = platform.y_max).  Anchors are the bottom-left-front corner of
// the model in world space.
//
//   Platform 1 (NW, y=[20,30)): key stake at y=30 → anchor y=30
//   Platform 2 (NE, y=[30,40)): key stake at y=40 → anchor y=40
//   Platform 3 (SE, y=[40,50)): key stake at y=50 → anchor y=50
//   Platform 4 (SW, y=[50,60)): key stake at y=60 → anchor y=60
//
// The goal totem (3×5×3) sits atop the goal tower (platform 5, y=[60,70)).
constexpr double kKeyAnchorData[4][3] = {
    { 119.5, 30.0, 119.5 },   // NW key
    { 379.5, 40.0, 119.5 },   // NE key
    { 379.5, 50.0, 379.5 },   // SE key
    { 119.5, 60.0, 379.5 },   // SW key
};
constexpr double kGoalAnchorData[3] = { 248.5, 70.0, 248.5 };

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// ── Asset generation ─────────────────────────────────────────────────────────

// Generate a 1×2×1 gold key stake .vox file.
// Uses palette index 12 (goal-gold, yellow) so the authored color round-trips
// correctly through Engine::importVox → palette::setColor → Engine::exportVox.
bool generateKeyAsset(const std::string& path) {
    LayerDef def;
    def.name              = "key";
    def.voxel_size_m      = 1.0;
    def.mode              = VoxelMode::terminal;
    def.chunk_size_voxels = 4;
    def.view_distance_chunks = 1;

    World w(def);
    Layer* lay = w.layer("key");
    if (!lay) return false;
    lay->loadChunk({0, 0, 0}, nullptr);

    Voxel gold;
    gold.material.palette_index       = 12;  // goal-gold
    gold.material.density             = 100.0f;
    gold.material.structural_strength = 1.0f;
    gold.material.hardness            = 0.2f;
    lay->setVoxel(WorldCoord(0.5, 0.5, 0.5), gold);
    lay->setVoxel(WorldCoord(0.5, 1.5, 0.5), gold);

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    VoxExporter exporter;
    return exporter.save(path, *lay, WorldCoord(0.0, 0.0, 0.0), WorldCoord(1.0, 2.0, 1.0));
}

// Generate a 3×5×3 goal totem .vox file.
// Body uses palette index 14 (brick-red props), apex uses index 12 (gold).
bool generateGoalAsset(const std::string& path) {
    LayerDef def;
    def.name              = "goal";
    def.voxel_size_m      = 1.0;
    def.mode              = VoxelMode::terminal;
    def.chunk_size_voxels = 8;
    def.view_distance_chunks = 1;

    World w(def);
    Layer* lay = w.layer("goal");
    if (!lay) return false;
    lay->loadChunk({0, 0, 0}, nullptr);

    Voxel body;
    body.material.palette_index       = 14;  // props (brick-red)
    body.material.density             = 1800.0f;
    body.material.structural_strength = 1.0f;
    body.material.hardness            = 0.8f;

    Voxel apex;
    apex.material.palette_index       = 12;  // goal-gold
    apex.material.density             = 100.0f;
    apex.material.structural_strength = 1.0f;
    apex.material.hardness            = 0.2f;

    // 3×4×3 body column
    for (int y = 0; y < 4; ++y)
        for (int z = 0; z < 3; ++z)
            for (int x = 0; x < 3; ++x)
                lay->setVoxel(WorldCoord(x + 0.5, y + 0.5, z + 0.5), body);
    // Gold apex (centre top)
    lay->setVoxel(WorldCoord(1.5, 4.5, 1.5), apex);

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    VoxExporter exporter;
    return exporter.save(path, *lay, WorldCoord(0.0, 0.0, 0.0), WorldCoord(3.0, 5.0, 3.0));
}

// ── Helper ────────────────────────────────────────────────────────────────────

// Returns the detail-layer child chunks that cover one terrace macro voxel.
// With terraces at 10 m and detail chunks at 10 m (chunk_size=10, vs=1),
// span = round(10 / 10) = 1 → one detail chunk per terrace voxel.
std::vector<ChunkCoord> childChunksForMacro(const chunkmath::VoxelCoord& macro,
                                            const Layer& parent, const Layer& child) {
    const double parentVoxel   = parent.voxelSizeM();
    const double childChunkSz  = child.voxelSizeM() * child.chunkSizeVoxels();
    const int    span = std::max(1, static_cast<int>(std::llround(parentVoxel / childChunkSz)));
    const WorldCoord origin = chunkmath::voxelOrigin(macro, parentVoxel);
    const ChunkCoord base   =
        chunkmath::worldToChunk(origin, child.voxelSizeM(), child.chunkSizeVoxels());

    std::vector<ChunkCoord> out;
    out.reserve(static_cast<size_t>(span) * span * span);
    for (int dz = 0; dz < span; ++dz)
        for (int dy = 0; dy < span; ++dy)
            for (int dx = 0; dx < span; ++dx)
                out.push_back(ChunkCoord{base.x + dx, base.y + dy, base.z + dz});
    return out;
}

}  // namespace

int main() {
    // ── Startup sequence (ORDER MATTERS) ──────────────────────────────────────
    // The phases below must run in this exact order; several have hard
    // dependencies on an earlier phase, and getting them out of order tends to
    // fail silently (no error, no window — just a crash on launch).
    //
    //   1. Load layer config            — pure data, no dependencies.
    //   2. Load plugins + look up gens   — needed before any world generation.
    //   3. Engine::init + World          — wires plugins/world; registers the
    //                                      built-in importers/exporters.
    //   4. Import key/goal .vox models   — populates detail-layer chunks. CPU
    //                                      only (voxel data); no GPU work yet.
    //   5. Persistence / LOD / worker    — plain CPU objects.
    //   6. Window + renderer.initialize  — *** bgfx::init happens HERE. ***
    //   7. Build GPU meshes              — ChunkMesh::build allocates bgfx
    //                                      vertex/index buffers, so it MUST run
    //                                      after phase 6. Building any mesh
    //                                      (including the imported key/goal
    //                                      chunks) before bgfx::init dereferences
    //                                      an uninitialized bgfx context and
    //                                      crashes with an access violation
    //                                      before the window ever appears.
    //   8. Main loop                     — stream, decompose, edit, render.
    //
    // Rule of thumb: nothing that touches bgfx (any ChunkMesh::build / renderChunk
    // / renderer.* call) may run before renderer.initialize() in phase 6.

    // ── Five-layer arena config ───────────────────────────────────────────────
    // Ratio chain: foundation(500)/ramparts(20)=25, ramparts(20)/terraces(10)=2,
    //              terraces(10)/props(2)=5, props(2)/detail(1)=2. All validated.
    LayerConfig layerConfig = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: foundation
    voxel_size_m: 500.0
    mode: immutable
    chunk_size_voxels: 4
    view_distance_chunks: 1
  - name: ramparts
    voxel_size_m: 20.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 4
  - name: terraces
    voxel_size_m: 10.0
    mode: composite
    decompose_to: detail
    chunk_size_voxels: 8
    view_distance_chunks: 4
  - name: props
    voxel_size_m: 2.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 8
  - name: detail
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 10
    view_distance_chunks: 12
)");
        } catch (const std::exception& e) {
            std::cerr << "[main] Fatal: layer config error: " << e.what() << "\n";
            std::exit(1);
        }
    }();

    // ── Arena plugin ─────────────────────────────────────────────────────────
    PluginManager pluginManager;
    if (std::string(VOXEL_ARENA_PLUGIN_PATH).empty()) {
        std::cerr << "[main] Fatal: arena plugin path not configured at build time.\n";
        return 1;
    }
    if (pluginManager.loadPlugin(VOXEL_ARENA_PLUGIN_PATH) == kInvalidPluginId) {
        std::cerr << "[main] Fatal: could not load arena plugin from "
                  << VOXEL_ARENA_PLUGIN_PATH << "\n";
        return 1;
    }

    // Look up each layer generator by name.
    auto findGen = [&](const std::string& name) -> RegisteredLayerGenerator {
        for (const auto& g : pluginManager.layerGenerators())
            if (g.layer_name == name) return g;
        return RegisteredLayerGenerator{name, nullptr, nullptr, kInvalidPluginId};
    };
    const auto foundationGen = findGen("foundation");
    const auto rampartsGen   = findGen("ramparts");
    const auto terracesGen   = findGen("terraces");
    const auto propsGen      = findGen("props");
    const auto detailGen     = findGen("detail");

    if (!foundationGen.fn || !rampartsGen.fn || !terracesGen.fn ||
        !propsGen.fn      || !detailGen.fn) {
        std::cerr << "[main] Fatal: arena plugin did not register all five "
                     "layer generators.\n";
        return 1;
    }

    // Build-material palette: the first nine arena materials are selectable (keys
    // 1-9). Capture their ids once at startup; props are resolved at placement
    // time via PluginManager::material(). This keeps selection valid across the
    // runtime hazards-plugin load/unload (P) — which mutates the registry vector —
    // instead of indexing it positionally with a possibly-stale index.
    std::vector<std::string> buildMaterials;
    for (size_t i = 0; i < pluginManager.materials().size() && i < 9; ++i)
        buildMaterials.push_back(pluginManager.materials()[i].material_id);
    if (buildMaterials.empty()) {
        std::cerr << "[main] Fatal: no materials registered by the arena plugin.\n";
        return 1;
    }
    size_t selectedMaterial = 0;
    std::cout << "[main] Build materials (press the number to select):\n";
    for (size_t i = 0; i < buildMaterials.size(); ++i)
        std::cout << "       " << (i + 1) << " - " << buildMaterials[i] << "\n";

    // ── Engine + window + renderer ────────────────────────────────────────────
    Engine engine;

    // ── World + layer handles ─────────────────────────────────────────────────
    World world(layerConfig);
    Layer* foundation = world.layer("foundation");
    Layer* ramparts   = world.layer("ramparts");
    Layer* terraces   = world.layer("terraces");
    Layer* props      = world.layer("props");
    Layer* detail     = world.layer("detail");
    if (!foundation || !ramparts || !terraces || !props || !detail) {
        std::cerr << "[main] Fatal: expected all five arena layers.\n";
        return 1;
    }

    // Phase 3: Engine::init wires the plugin manager and world into the engine and
    // registers the built-in VoxImporter / VoxExporter handlers needed by
    // importVox/exportVox.
    engine.init(pluginManager, world);
    engine.start();

    // ── Asset paths ───────────────────────────────────────────────────────────
    const std::string assetDir =
        (std::string(DEMO_ASSET_DIR)[0] != '\0') ? std::string(DEMO_ASSET_DIR) : ".";
    const std::string keyVoxPath  = assetDir + "/key.vox";
    const std::string goalVoxPath = assetDir + "/goal_totem.vox";

    if (!std::filesystem::exists(keyVoxPath)) {
        std::cout << "[main] Generating key asset: " << keyVoxPath << "\n";
        if (!generateKeyAsset(keyVoxPath))
            std::cerr << "[main] Warning: could not generate " << keyVoxPath << "\n";
    }
    if (!std::filesystem::exists(goalVoxPath)) {
        std::cout << "[main] Generating goal totem asset: " << goalVoxPath << "\n";
        if (!generateGoalAsset(goalVoxPath))
            std::cerr << "[main] Warning: could not generate " << goalVoxPath << "\n";
    }

    // ── Phase 4: Import key and goal totem models into the detail layer ───────
    // Populates detail-layer chunks with voxel data only — no GPU meshes are
    // built here (that is phase 7, after the renderer/bgfx exists).
    // Each key is a 1×2×1 gold stake placed just above the corresponding
    // platform's top surface.  VoxImporter creates the detail-layer chunk if it
    // is not yet resident, places the model voxels, and marks the chunk dirty so
    // it is persisted on the first eviction (the chunk keeps the key/goal models
    // across sessions).
    //
    // Key chunks sit ABOVE the terrace platform Y ranges (their parent terrace
    // macro voxel is empty), so they are never overwritten by the decomposition
    // worker.  They are registered as "persistent" chunks so the per-frame
    // eviction loop never drops them from memory.
    for (const auto& a : kKeyAnchorData)
        engine.importVox(keyVoxPath, "detail", WorldCoord(a[0], a[1], a[2]));
    engine.importVox(goalVoxPath, "detail",
                     WorldCoord(kGoalAnchorData[0], kGoalAnchorData[1], kGoalAnchorData[2]));

    // ── Persistence: dirty detail chunks survive across launches ─────────────
    const LayerDef* detailDef = layerConfig.findLayer("detail");
    persistence::WorldSave detailSave(
        "arena-save",
        persistence::WorldIdentity{detailDef->voxel_size_m, detailDef->chunk_size_voxels});
    std::cout << "[main] Save directory: " << detailSave.directory() << "\n";

    // ── LOD + decomposition ───────────────────────────────────────────────────
    LODManager lod(layerConfig);
    lod.setVerticalBand(-1, 0);

    DecompositionState  decomp;
    DecompositionWorker worker;
    std::cout << "[main] Decomposition worker threads: " << worker.threadCount() << "\n";

    // Mesh stores: one per layer that this demo renders directly.
    MeshStore foundationMeshes;
    MeshStore rampartsMeshes;
    MeshStore terracesMeshes;
    MeshStore propsMeshes;
    MeshStore detailMeshes;     // populated via decomposition results + key/goal import

    // Chunks created by VoxImporter (keys/goal) — never evicted during normal play.
    // These chunks sit above the terrace platforms and are invisible to the
    // decomposition worker, so there is no overwrite risk from terrace decomposition.
    std::unordered_set<ChunkCoord, ChunkCoordHash> persistentChunks;

    // NOTE: meshes for these imported chunks are built later, in phase 7 (after
    // renderer.initialize() below) — ChunkMesh::build allocates bgfx buffers,
    // which crashes if run before bgfx::init. Do NOT build them here.

    // Template voxel captured from the first decomposed terrace, used to restore
    // macro voxels when decomposed detail drifts past kDetailKeepRadiusM.
    Voxel terraceTemplate;
    bool  haveTerraceTemplate = false;

    // ── Apply feature generators to a detail chunk ────────────────────────────
    auto applyFeatures = [&](Chunk& chunk) {
        for (const auto& f : pluginManager.featureGenerators())
            if (f.fn)
                f.fn(chunk.origin(), detail->voxelSizeM(), detail->chunkSizeVoxels(),
                     chunk.data(), nullptr, 0, 0u, f.user_data);
    };

    // ── Detail layer edit helpers ─────────────────────────────────────────────
    auto remeshDetailChunk = [&](const ChunkCoord& cc) {
        const Chunk* chunk = detail->getChunk(cc);
        if (!chunk) return;
        auto it = detailMeshes.find(cc);
        if (it != detailMeshes.end()) {
            it->second.destroy();
            it->second = ChunkMesh::build(*chunk);
        } else {
            detailMeshes.emplace(cc, ChunkMesh::build(*chunk));
        }
    };

    auto editDetailVoxel = [&](const chunkmath::VoxelCoord& vc, const Voxel& newVox) {
        const WorldCoord center = chunkmath::voxelCenter(vc, detail->voxelSizeM());
        const Voxel oldVox = world.getVoxel(center);
        if (!world.setVoxel(center, newVox)) return;
        for (const auto& h : pluginManager.voxelModifiedHooks())
            if (h.fn) h.fn(center, &oldVox, &newVox, kLocalPlayer, h.user_data);
        remeshDetailChunk(chunkmath::voxelToChunkLocal(vc, detail->chunkSizeVoxels()).chunk);
    };

    // ── Hazards plugin state ──────────────────────────────────────────────────
    PluginId hazardsPluginId = kInvalidPluginId;

    // ── Game objective state ──────────────────────────────────────────────────
    bool keysCollected[4] = {};
    bool gameWon          = false;

    // ── Camera / player state ─────────────────────────────────────────────────
    float      pitch = -0.25f, yaw = 0.0f;
    WorldCoord camPos(kSpawnPos);
    double     lastMX = 0.0, lastMY = 0.0;
    bool       firstMouse = true;
    bool       cursorCaptured = true;
    bool       prevKeyF = false, prevKeyG = false;
    bool       prevKeyP = false, prevKeyE = false;
    bool       prevLeft = false, prevRight = false;

    bool       walkMode = false;
    WorldCoord playerCenter(0.0, 0.0, 0.0);
    double     vy = 0.0;
    bool       grounded = false;

    // ── Phase 6: Window + renderer ── bgfx::init happens inside initialize() ──
    // Every ChunkMesh::build / renderer.* call below depends on this having run.
    platform::Window window(1280, 720, "VoxelEngine — M7b Arena Platformer");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    // ── Phase 7: Build GPU meshes for the imported (key/goal) chunks ──────────
    // This MUST come after phase 6: ChunkMesh::build allocates bgfx vertex/index
    // buffers, so running it before bgfx::init dereferences an uninitialized bgfx
    // context and crashes with an access violation — before the window appears,
    // so it looks like the program "runs and closes" with no error and no window.
    // (Every other mesh in this demo is built inside the main loop, i.e. after
    // this point; these imported chunks are the only ones that exist pre-loop.)
    for (const auto& [coord, chunkPtr] : detail->chunks()) {
        detailMeshes.emplace(coord, ChunkMesh::build(*chunkPtr));
        persistentChunks.insert(coord);
    }

    // ── HUD: on-screen key counter ────────────────────────────────────────────
    // Refresh the top-left overlay from the current objective state. Called once
    // up front and again whenever the count changes (collect / win).
    auto updateHud = [&] {
        int collected = 0;
        for (bool k : keysCollected) collected += k ? 1 : 0;
        std::string line = "Keys: " + std::to_string(collected) + " / 4";
        if (gameWon)            line += "   *** YOU WIN! ***";
        else if (collected == 4) line += "   Reach the goal totem!";
        renderer.setHudText({line});
    };
    updateHud();

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    std::cout << "[main] Arena platformer. Five-layer world: foundation (500m) + "
                 "ramparts (20m) + terraces (10m) + props (2m) + detail (1m).\n"
                 "[main] WASD + mouse to fly, Space/Shift up/down, G = walk "
                 "(cross-layer collision), F = cursor, ESC quits.\n"
                 "[main] Left mouse = break detail voxel, right mouse = place, "
                 "1-9 = select material.\n"
                 "[main] Fly toward platforms to decompose them; build bridges to "
                 "cross gaps. Edits save to arena-save/ and survive relaunch.\n"
                 "[main] In walk mode (G), head north from spawn to the stone "
                 "staircase and jump up its steps onto the start pad.\n"
                 "[main] Collect the four gold key stakes (walk into them), then "
                 "reach the goal totem to win.\n"
                 "[main] P = toggle lava hazards on platforms, E = export detail "
                 "layer to arena-export.vox.\n";

    while (!window.shouldClose()) {
        window.pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // F: toggle cursor capture.
        bool curKeyF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curKeyF && !prevKeyF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                             cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevKeyF = curKeyF;

        // G: toggle walk / fly.
        bool curKeyG = (glfwGetKey(glfwWin, GLFW_KEY_G) == GLFW_PRESS);
        if (curKeyG && !prevKeyG) {
            walkMode = !walkMode;
            if (walkMode) {
                playerCenter = WorldCoord(camPos.value - glm::dvec3(0.0, kEyeOffset, 0.0));
                vy = 0.0; grounded = false;
            }
            std::cout << "[main] Mode: "
                      << (walkMode ? "WALK (gravity + cross-layer collision)" : "FLY")
                      << "\n";
        }
        prevKeyG = curKeyG;

        // P: toggle hazards plugin (lava pools on platform surfaces).
        bool curKeyP = (glfwGetKey(glfwWin, GLFW_KEY_P) == GLFW_PRESS);
        if (curKeyP && !prevKeyP) {
            if (hazardsPluginId != kInvalidPluginId) {
                pluginManager.unloadPlugin(hazardsPluginId);
                hazardsPluginId = kInvalidPluginId;
                std::cout << "[main] Hazards unloaded — arena regenerating clean.\n";
            } else if (std::string(VOXEL_HAZARDS_PLUGIN_PATH)[0] != '\0') {
                hazardsPluginId = pluginManager.loadPlugin(VOXEL_HAZARDS_PLUGIN_PATH);
                if (hazardsPluginId != kInvalidPluginId)
                    std::cout << "[main] Hazards loaded — lava pools on platforms on approach.\n";
                else
                    std::cerr << "[main] Warning: could not load hazards plugin.\n";
            } else {
                std::cerr << "[main] Warning: hazards plugin path not configured at build time.\n";
            }
            // Evict all non-persistent detail chunks so they regenerate fresh with
            // or without the hazard feature, reverting the arena exactly (M4 pattern).
            {
                std::vector<ChunkCoord> toEvict;
                for (const auto& kv : detailMeshes)
                    if (!persistentChunks.count(kv.first))
                        toEvict.push_back(kv.first);
                std::unordered_set<ChunkCoord, ChunkCoordHash> toRemesh;
                for (const ChunkCoord& dc : toEvict) {
                    if (world.isChunkDirty(dc)) {
                        if (const Chunk* ch = world.getChunk(dc)) detailSave.saveChunk(*ch);
                        world.clearChunkDirty(dc);
                    }
                    auto it = detailMeshes.find(dc);
                    if (it != detailMeshes.end()) { it->second.destroy(); detailMeshes.erase(it); }
                    detail->unloadChunk(dc);
                    const chunkmath::VoxelCoord V{dc.x, dc.y, dc.z};
                    const bool wasDecomposed = decomp.isDecomposed(V);
                    decomp.clear(V);
                    if (wasDecomposed && haveTerraceTemplate) {
                        const chunkmath::LocalVoxel lv =
                            chunkmath::voxelToChunkLocal(V, terraces->chunkSizeVoxels());
                        auto cit = terraces->chunks().find(lv.chunk);
                        if (cit != terraces->chunks().end()) {
                            cit->second->at(lv.x, lv.y, lv.z) = terraceTemplate;
                            toRemesh.insert(lv.chunk);
                        }
                    }
                }
                for (const ChunkCoord& c : toRemesh) {
                    const Chunk* chunk = terraces->getChunk(c);
                    if (!chunk) continue;
                    auto it = terracesMeshes.find(c);
                    if (it != terracesMeshes.end()) {
                        it->second.destroy();
                        it->second = ChunkMesh::build(*chunk);
                    }
                }
            }
        }
        prevKeyP = curKeyP;

        // E: export the detail layer to arena-export.vox.
        // The 500-voxel-wide region (>256 per axis) exercises auto-chunking and
        // the lossy-property warning path (arena voxels carry non-default density).
        bool curKeyE = (glfwGetKey(glfwWin, GLFW_KEY_E) == GLFW_PRESS);
        if (curKeyE && !prevKeyE) {
            const std::string exportPath = "arena-export.vox";
            std::cout << "[main] Exporting detail layer to " << exportPath << " …\n";
            engine.exportVox("detail",
                             WorldCoord(0.0, 0.0, 0.0), WorldCoord(500.0, 80.0, 500.0),
                             exportPath);
            std::cout << "[main] Export done. (500×80×500 → auto-chunked; "
                         "extended properties → lossy-property warning logged)\n";
        }
        prevKeyE = curKeyE;

        // Material selection (1-9): choose what the right mouse places.
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
                yaw   += static_cast<float>(mx - lastMX) * kMouseSens;
                pitch -= static_cast<float>(my - lastMY) * kMouseSens;
                if (pitch >  1.55f) pitch =  1.55f;
                if (pitch < -1.55f) pitch = -1.55f;
            }
            lastMX = mx; lastMY = my; firstMouse = false;
        }

        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);

        if (!walkMode) {
            glm::dvec3 fwd  {double(cp * sy), double(sp), double(cp * cy)};
            glm::dvec3 right{double(cy),      0.0,        double(-sy)};
            glm::dvec3 delta{};
            double step = double(kFlySpeed * dt);
            if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += fwd   * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= fwd   * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) delta -= right * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) delta += right * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) delta.y += step;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) delta.y -= step;
            camPos = WorldCoord(camPos.value + delta);
        } else {
            glm::dvec3 fwdH  {double(sy),  0.0, double(cy)};
            glm::dvec3 rightH{double(cy),  0.0, double(-sy)};
            glm::dvec3 wish{};
            if (glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS) wish += fwdH;
            if (glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS) wish -= fwdH;
            if (glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS) wish -= rightH;
            if (glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS) wish += rightH;
            if (glm::length(wish) > 0.0) wish = glm::normalize(wish);
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE) == GLFW_PRESS && grounded)
                vy = kJumpSpeed;
            vy -= kGravity * double(dt);
            glm::dvec3 delta = wish * (kWalkSpeed * double(dt));
            delta.y += vy * double(dt);
            voxelcollide::MoveResult mr =
                voxelcollide::moveAABB(world, {playerCenter, kPlayerHalf}, delta);
            playerCenter = mr.position;
            grounded     = mr.grounded;
            if (mr.grounded || (mr.hitY && vy > 0.0)) vy = 0.0;
            camPos = WorldCoord(playerCenter.value + glm::dvec3(0.0, kEyeOffset, 0.0));
        }

        // ── Game-objective logic (walk mode only) ─────────────────────────────
        if (walkMode && !gameWon) {
            // Key collection: check proximity to each uncollected key stake.
            // Trigger volume is a 2×3×2 m box centred on the key stake.
            int collectedCount = 0;
            for (int i = 0; i < 4; ++i) {
                if (keysCollected[i]) { ++collectedCount; continue; }
                const glm::dvec3 keyCenter(
                    kKeyAnchorData[i][0] + 0.5, // key model is 1 wide → centre at +0.5
                    kKeyAnchorData[i][1] + 1.0, // key model is 2 tall → centre at +1.0
                    kKeyAnchorData[i][2] + 0.5);
                const glm::dvec3 d = playerCenter.value - keyCenter;
                if (std::abs(d.x) < 2.0 && std::abs(d.y) < 3.0 && std::abs(d.z) < 2.0) {
                    keysCollected[i] = true;
                    ++collectedCount;
                    // Clear the key stake voxels from the detail layer.
                    world.setVoxel(WorldCoord(kKeyAnchorData[i][0] + 0.5,
                                              kKeyAnchorData[i][1] + 0.5,
                                              kKeyAnchorData[i][2] + 0.5), Voxel::empty());
                    world.setVoxel(WorldCoord(kKeyAnchorData[i][0] + 0.5,
                                              kKeyAnchorData[i][1] + 1.5,
                                              kKeyAnchorData[i][2] + 0.5), Voxel::empty());
                    // Rebuild mesh for the key chunk.
                    const ChunkCoord kc = chunkmath::worldToChunk(
                        WorldCoord(kKeyAnchorData[i][0] + 0.5,
                                   kKeyAnchorData[i][1] + 0.5,
                                   kKeyAnchorData[i][2] + 0.5),
                        detail->voxelSizeM(), detail->chunkSizeVoxels());
                    remeshDetailChunk(kc);
                    const int remaining = 4 - collectedCount;
                    std::cout << "[main] Key " << (i + 1)
                              << " collected! " << remaining << " remaining.\n";
                    updateHud();
                }
            }

            // Win condition: all keys collected + player near goal totem.
            if (collectedCount == 4) {
                const glm::dvec3 goalCenter(
                    kGoalAnchorData[0] + 1.5,   // goal model is 3 wide → centre at +1.5
                    kGoalAnchorData[1] + 2.5,   // goal model is 5 tall → centre at +2.5
                    kGoalAnchorData[2] + 1.5);
                const glm::dvec3 d = playerCenter.value - goalCenter;
                if (std::abs(d.x) < 3.0 && std::abs(d.y) < 4.0 && std::abs(d.z) < 3.0) {
                    gameWon = true;
                    std::cout << "[main] *** VICTORY!  All keys collected and "
                                 "goal totem reached! ***\n";
                    updateHud();
                }
            }

            // Respawn: fall below the arena floor.
            if (playerCenter.value.y < -5.0) {
                playerCenter = WorldCoord(kSpawnPos);
                camPos = WorldCoord(kSpawnPos + glm::dvec3(0.0, kEyeOffset, 0.0));
                vy = 0.0; grounded = false;
                std::cout << "[main] Respawned! (fell off the arena)\n";
            }

            // Respawn: touch a lava voxel (sampled just below the player's feet).
            if (!gameWon) {
                const WorldCoord feetSample(
                    playerCenter.value - glm::dvec3(0.0, kPlayerHalf.y + 0.1, 0.0));
                const Voxel underFeet = world.getVoxel(feetSample);
                if (!underFeet.isEmpty() && underFeet.material.palette_index == kLavaIdx) {
                    playerCenter = WorldCoord(kSpawnPos);
                    camPos = WorldCoord(kSpawnPos + glm::dvec3(0.0, kEyeOffset, 0.0));
                    vy = 0.0; grounded = false;
                    std::cout << "[main] Respawned! (touched lava)\n";
                }
            }
        }

        // ── Stream immutable and composite layers around the camera ───────────
        auto stream = [&](Layer* layer, const std::string& name, MeshStore& meshes,
                          const RegisteredLayerGenerator& gen) {
            const ChunkCoord center =
                chunkmath::worldToChunk(camPos, layer->voxelSizeM(), layer->chunkSizeVoxels());
            int loaded = 0;
            for (const ChunkCoord& c : lod.desiredChunks(center, name)) {
                if (meshes.count(c)) continue;
                Chunk* chunk = layer->loadChunk(c, gen.fn, gen.user_data);
                if (!chunk) continue;
                meshes.emplace(c, ChunkMesh::build(*chunk));
                if (++loaded >= kStreamPerFrame) break;
            }
            const bool isTerraces = (layer == terraces);
            std::vector<ChunkCoord> toEvict;
            for (const auto& kv : meshes)
                if (lod.shouldEvict(center, kv.first, name)) toEvict.push_back(kv.first);
            for (const ChunkCoord& c : toEvict) {
                if (isTerraces) {
                    const int csz = layer->chunkSizeVoxels();
                    bool anyPending = false;
                    for (int z = 0; z < csz && !anyPending; ++z)
                        for (int y = 0; y < csz && !anyPending; ++y)
                            for (int x = 0; x < csz && !anyPending; ++x)
                                if (decomp.isPending(
                                        chunkmath::chunkLocalToVoxel(c, x, y, z, csz)))
                                    anyPending = true;
                    if (anyPending) continue;
                    const int n = layer->chunkSizeVoxels();
                    for (int z = 0; z < n; ++z)
                        for (int y = 0; y < n; ++y)
                            for (int x = 0; x < n; ++x) {
                                const chunkmath::VoxelCoord V =
                                    chunkmath::chunkLocalToVoxel(c, x, y, z, n);
                                if (!decomp.isDecomposed(V)) continue;
                                for (const ChunkCoord& dc :
                                         childChunksForMacro(V, *terraces, *detail)) {
                                    if (persistentChunks.count(dc)) continue;
                                    auto it = detailMeshes.find(dc);
                                    if (it != detailMeshes.end()) {
                                        it->second.destroy();
                                        detailMeshes.erase(it);
                                    }
                                    if (world.isChunkDirty(dc)) {
                                        if (const Chunk* ch = world.getChunk(dc))
                                            detailSave.saveChunk(*ch);
                                        world.clearChunkDirty(dc);
                                    }
                                    detail->unloadChunk(dc);
                                }
                                decomp.clear(V);
                            }
                }
                meshes[c].destroy();
                meshes.erase(c);
                layer->unloadChunk(c);
            }
        };

        stream(foundation, "foundation", foundationMeshes, foundationGen);
        stream(ramparts,   "ramparts",   rampartsMeshes,   rampartsGen);
        stream(props,      "props",      propsMeshes,       propsGen);
        stream(terraces,   "terraces",   terracesMeshes,   terracesGen);

        // ── Trigger decomposition for nearby terrace macro voxels ─────────────
        int enqueued = 0;
        for (const auto& kv : terracesMeshes) {
            if (enqueued >= kDecomposePerFrame) break;
            const Chunk* chunk = terraces->getChunk(kv.first);
            if (!chunk) continue;
            const int n = chunk->size();
            for (int z = 0; z < n && enqueued < kDecomposePerFrame; ++z)
                for (int y = 0; y < n && enqueued < kDecomposePerFrame; ++y)
                    for (int x = 0; x < n && enqueued < kDecomposePerFrame; ++x) {
                        if (chunk->at(x, y, z).isEmpty()) continue;
                        const chunkmath::VoxelCoord V =
                            chunkmath::chunkLocalToVoxel(kv.first, x, y, z, n);
                        if (!decomp.needsDecompose(V)) continue;
                        const WorldCoord ctr = chunkmath::voxelCenter(V, terraces->voxelSizeM());
                        if (glm::length(ctr.value - camPos.value) > kDecomposeRadiusM) continue;
                        if (!decomp.markPending(V)) continue;
                        DecompositionJob job;
                        job.macro           = V;
                        job.childChunks     = childChunksForMacro(V, *terraces, *detail);
                        job.childChunkSize  = detail->chunkSizeVoxels();
                        job.childVoxelSizeM = detail->voxelSizeM();
                        job.generator       = detailGen.fn;
                        job.userData        = detailGen.user_data;
                        worker.enqueue(job);
                        ++enqueued;
                    }
        }

        // ── Integrate completed decomposition results ─────────────────────────
        std::unordered_set<ChunkCoord, ChunkCoordHash> terracesToRemesh;
        for (DecompositionResult& result : worker.drain()) {
            for (auto& chunkPtr : result.chunks) {
                const ChunkCoord dc = chunkPtr->coord();
                // Prefer a saved (player-edited) chunk over the generator output.
                bool fromSave = false;
                if (detailSave.hasChunk(dc)) {
                    if (auto saved = detailSave.tryLoadChunk(dc)) {
                        chunkPtr  = std::move(saved);
                        fromSave  = true;
                    }
                }
                // Skip if a persistent chunk (key/goal model) already occupies this coord.
                if (persistentChunks.count(dc)) continue;
                Chunk* inserted = detail->insertChunk(std::move(chunkPtr));
                if (!inserted) continue;
                if (!fromSave) applyFeatures(*inserted);
                auto it = detailMeshes.find(dc);
                if (it != detailMeshes.end()) {
                    it->second.destroy();
                    it->second = ChunkMesh::build(*inserted);
                } else {
                    detailMeshes.emplace(dc, ChunkMesh::build(*inserted));
                }
            }
            // Clear the decomposed macro voxel so it stops rendering as a block.
            const chunkmath::LocalVoxel lv =
                chunkmath::voxelToChunkLocal(result.macro, terraces->chunkSizeVoxels());
            auto cit = terraces->chunks().find(lv.chunk);
            if (cit != terraces->chunks().end()) {
                if (!haveTerraceTemplate) {
                    terraceTemplate     = cit->second->at(lv.x, lv.y, lv.z);
                    haveTerraceTemplate = true;
                }
                cit->second->at(lv.x, lv.y, lv.z) = Voxel::empty();
                terracesToRemesh.insert(lv.chunk);
            }
            decomp.markDecomposed(result.macro);
        }

        // ── Release detail chunks that have drifted past the keep radius ──────
        std::vector<ChunkCoord> detailToEvict;
        for (const auto& kv : detailMeshes) {
            if (persistentChunks.count(kv.first)) continue;  // never evict key/goal chunks
            const chunkmath::VoxelCoord V{kv.first.x, kv.first.y, kv.first.z};
            const WorldCoord ctr = chunkmath::voxelCenter(V, terraces->voxelSizeM());
            if (glm::length(ctr.value - camPos.value) > kDetailKeepRadiusM)
                detailToEvict.push_back(kv.first);
        }
        for (const ChunkCoord& dc : detailToEvict) {
            if (world.isChunkDirty(dc)) {
                if (const Chunk* ch = world.getChunk(dc)) detailSave.saveChunk(*ch);
                world.clearChunkDirty(dc);
            }
            auto it = detailMeshes.find(dc);
            if (it != detailMeshes.end()) {
                it->second.destroy();
                detailMeshes.erase(it);
            }
            detail->unloadChunk(dc);
            const chunkmath::VoxelCoord V{dc.x, dc.y, dc.z};
            // Only restore the terrace macro voxel if this chunk came from decomposition.
            const bool wasDecomposed = decomp.isDecomposed(V);
            decomp.clear(V);
            if (wasDecomposed && haveTerraceTemplate) {
                const chunkmath::LocalVoxel lv =
                    chunkmath::voxelToChunkLocal(V, terraces->chunkSizeVoxels());
                auto cit = terraces->chunks().find(lv.chunk);
                if (cit != terraces->chunks().end()) {
                    cit->second->at(lv.x, lv.y, lv.z) = terraceTemplate;
                    terracesToRemesh.insert(lv.chunk);
                }
            }
        }

        for (const ChunkCoord& c : terracesToRemesh) {
            const Chunk* chunk = terraces->getChunk(c);
            if (!chunk) continue;
            auto it = terracesMeshes.find(c);
            if (it != terracesMeshes.end()) {
                it->second.destroy();
                it->second = ChunkMesh::build(*chunk);
            }
        }

        // ── Targeting and edits (detail layer) ───────────────────────────────
        glm::dvec3 lookDir{double(cp * sy), double(sp), double(cp * cy)};
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);
        if (hit.hit)
            renderer.drawVoxelHighlight(
                chunkmath::voxelCenter(hit.voxel, detail->voxelSizeM()),
                static_cast<float>(detail->voxelSizeM()));

        bool curLeft  = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS);
        bool curRight = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        if (hit.hit && curLeft && !prevLeft) {
            editDetailVoxel(hit.voxel, Voxel::empty());  // break
        }
        if (hit.hit && curRight && !prevRight) {
            const double vs = detail->voxelSizeM();
            const chunkmath::VoxelCoord t = hit.adjacent;
            bool blockedByPlayer;
            if (walkMode) {
                glm::dvec3 cmin{double(t.x) * vs, double(t.y) * vs, double(t.z) * vs};
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
                // The placement target may be an empty cell in a detail chunk that
                // is not resident — most commonly the air directly above a platform,
                // since every platform top sits on a detail-chunk boundary (tops at
                // y = 20/30/…/70, chunks 10 m tall). Layer::setVoxel fails silently
                // on a missing chunk, so create an empty one first. It is not backed
                // by terrace decomposition (its parent macro voxel is empty and never
                // decomposes), so mark it persistent — like the imported key/goal
                // chunks — so the per-frame radius eviction never drops the built
                // voxel mid-session.
                const ChunkCoord tc =
                    chunkmath::voxelToChunkLocal(t, detail->chunkSizeVoxels()).chunk;
                if (!detail->getChunk(tc)) {
                    detail->loadChunk(tc, nullptr);  // empty chunk
                    persistentChunks.insert(tc);
                }
                Voxel placed;
                placed.material = pluginManager.material(buildMaterials[selectedMaterial]);
                editDetailVoxel(t, placed);
            }
        }
        prevLeft  = curLeft;
        prevRight = curRight;

        // ── Resize ────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        // ── Render every layer at its own voxel scale ─────────────────────────
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        for (const auto& kv : foundationMeshes) {
            const Chunk* c = foundation->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), foundation->voxelSizeM(), foundation->chunkSizeVoxels());
        }
        for (const auto& kv : rampartsMeshes) {
            const Chunk* c = ramparts->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), ramparts->voxelSizeM(), ramparts->chunkSizeVoxels());
        }
        for (const auto& kv : propsMeshes) {
            const Chunk* c = props->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), props->voxelSizeM(), props->chunkSizeVoxels());
        }
        for (const auto& kv : terracesMeshes) {
            const Chunk* c = terraces->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), terraces->voxelSizeM(), terraces->chunkSizeVoxels());
        }
        for (const auto& kv : detailMeshes) {
            const Chunk* c = detail->getChunk(kv.first);
            if (c) renderer.renderChunk(kv.second, c->origin(), detail->voxelSizeM(), detail->chunkSizeVoxels());
        }

        renderer.render();
    }

    std::cout << "[main] Decomposed terrace macro voxels this session: "
              << decomp.decomposedCount() << "\n";

    // Persist any edited detail chunks still in memory.
    int savedOnQuit = detailSave.saveDirtyChunks(world);
    if (savedOnQuit > 0)
        std::cout << "[main] Saved " << savedOnQuit << " edited detail chunk(s) to "
                  << detailSave.directory() << "\n";

    for (auto& kv : foundationMeshes) kv.second.destroy();
    for (auto& kv : rampartsMeshes)   kv.second.destroy();
    for (auto& kv : propsMeshes)      kv.second.destroy();
    for (auto& kv : terracesMeshes)   kv.second.destroy();
    for (auto& kv : detailMeshes)     kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

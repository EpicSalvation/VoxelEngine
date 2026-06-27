// M17 demo — multi-level (grandparent) structural collapse.
//
// The visible home for M17's headline engineering item: multi-level upward damage
// propagation (gap audit G1, docs/ARCHITECTURE.md §7). M13 resolved structural
// stability at a SINGLE composite level (demo 13). M17 walks the FULL ancestor
// chain the M10 cascade computes, so a *grandchild* edit re-evaluates not just its
// parent macro but every coarser ancestor. This demo is the realization of
// tests/MultiLevelPropagationTest.cpp's macro→micro→grid stack as a scene you can
// fly into and mine.
//
//   • demo 10 has a genuinely deep composite stack but never builds a PhysicsSystem.
//   • demo 13 builds a PhysicsSystem but over ONE composite level (blocks→terrain).
// This demo combines them: a deep stack + PhysicsSystem + a crumble response, where
// mining fine GRID (1 m) voxels — the grandchildren — hollows a MICRO parent, which
// lowers a MACRO grandparent's aggregate, until the grandparent caves in and the
// collapse cascades macro-by-macro, STOPPING at the part still anchored to a tower.
//
// The scene mirrors demo 13's stone bridge, two levels deeper: an 8-macro stone
// deck (4 m grandparent voxels, each = 4×4×4 = 64 editable 1 m grid voxels) spans
// between two immutable BEDROCK towers. As built it is stable at the macro scale —
// every deck macro is within stone's support reach (~4.5 macros) of a tower. Mine
// the grid voxels of a deck macro near one tower: its aggregate falls, the support
// it relays weakens, and the far end of the deck loses its path to that tower. The
// engine fires a MACRO-scale (grandparent) structural event — an event the
// single-level M13 engine never produced — and the response clears it; the cascade
// re-dirties up the chain and marches outward, terminating at the macros still
// reachable from the OTHER tower.
//
// ── Why the response acts only at the grandparent scale ──────────────────────────
// The support flood runs INDEPENDENTLY at every composite level, and its span is
// measured in *voxels at that level* (tuning::physics::kSupportSpanPerStrength). So
// the same material reaches a SHORTER METRIC distance at a finer level: stone's
// ~4.5-voxel span is 4.5 macros (18 m) at the macro level but only 4.5 micros (9 m)
// at the micro level. A structure that is stable at the macro scale is therefore
// generally UNSTABLE at the micro scale — the doubly-anchored deck's middle micros
// cannot reach either tower within 4.5 micros. A response that acted at *every*
// scale (the generic `crumble` plugin) would let the micro level self-destruct the
// deck the instant physics runs. So this demo's response — registered in-process via
// PluginManager::wireInPlugin, the same mechanism the multi-level test uses — filters
// to the COARSEST (grandparent/macro) scale and clears the unstable macro's full
// 1 m grid footprint. That makes the macro the SOLE structural actor and the
// grid→micro→macro aggregation chain (not a finer span) the thing driving collapse —
// which is exactly the behavior M17's G1 work added. The engine still detects and
// fires at the micro scale too; the HUD counts those events so the upward chain is
// visible, but the demo's POLICY only collapses grandparents (a legitimate response
// choice — the engine ships no default collapse; §7).
//
// Controls: WASD + mouse-look fly, Space/Shift up/down, F toggles cursor,
//           HOLD LEFT MOUSE to mine the targeted 1 m grid voxel (chew a macro
//           hollow), RIGHT MOUSE knocks out the whole targeted 4 m macro at once,
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
#include "simulation/RemovalAccumulator.h"
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

using chunkmath::VoxelCoord;

namespace {

constexpr char kLogCat[] = "demo19";

constexpr float  kFlySpeed  = 20.0f;
constexpr float  kMouseSens = 0.002f;
constexpr double kReachM    = 44.0;   // generous reach so the whole deck is minable
constexpr float  kToolPower  = 12.0f; // grid-voxel removal speed (units/s)

// ── World layout (docs/ARCHITECTURE.md §7, gap audit G1) ─────────────────────────
// A deep composite chain mirroring tests/MultiLevelPropagationTest.cpp:
//   macro 4 m (composite→micro)  — the GRANDPARENT structural unit (level 1)
//   micro 2 m (composite→grid)   — the parent (level 0)
//   grid  1 m (terminal)         — the editable grandchildren the player mines
// Plus an immutable bedrock anchor layer (the towers + floor the cascade stops at).
// The macro chunk is 16 (64 m) so the whole scene — deck, towers, and the deck's
// perpendicular macro neighbors — is resident in one chunk: those neighbors read
// resident-empty (not anchors), so only the bedrock towers anchor the deck (the
// same residency setup the test relies on). macro→micro chunk-world size is 4 m
// (micro 2 m × chunk 2) == the 4 m macro voxel, satisfying the §4 coarse-supersets-
// fine rule.
const char* kLayerYaml = R"(
layers:
  - name: macro
    voxel_size_m: 4.0
    mode: composite
    decompose_to: micro
    chunk_size_voxels: 16
  - name: micro
    voxel_size_m: 2.0
    mode: composite
    decompose_to: grid
    chunk_size_voxels: 2
  - name: grid
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 2
  - name: bedrock
    voxel_size_m: 0.5
    mode: immutable
    chunk_size_voxels: 64
)";

// Structure geometry in MACRO coordinates (1 unit = one 4 m grandparent voxel),
// reusing demo 13's proven stable-yet-cascading bridge layout at the macro scale.
constexpr int64_t kMacroToGrid = 4;   // grid voxels per macro edge (4 m / 1 m)
constexpr int64_t kZ       = 8;             // 1 macro thick in z (clean side view)
constexpr int64_t kYFloor  = 2;             // bedrock floor macro-y
constexpr int64_t kYDeck   = 6;             // stone deck / tower-top macro-y
constexpr int64_t kXLTower = 3,  kXRTower = 12;   // the two bedrock anchor towers
constexpr int64_t kDeckX0  = 4,  kDeckX1  = 11;   // 8-macro stone deck between them

// stone: the deck material. structural_strength 0.9 → ~4.5-macro span, so an
// 8-macro deck anchored at BOTH towers is stable, but a run cut off from one tower
// past ~4 macros caves. Matches the material-showcase "stone" palette (gray).
Voxel stone() {
    Voxel v;
    v.material.density             = 2700.0f;
    v.material.structural_strength = 0.9f;
    v.material.hardness            = 0.7f;
    v.material.palette_index       = 1;   // gray
    return v;
}

// bedrock: the immutable anchor. Very high strength, indestructible; in its own
// immutable layer so picking/edits never touch it (the cascade stops here).
Voxel bedrock() {
    Voxel v;
    v.material.density             = 5000.0f;
    v.material.structural_strength = 9.9f;
    v.material.hardness            = -1.0f;  // indestructible sentinel
    v.material.palette_index       = 10;     // near-black
    return v;
}

// ── The demo's structural-response "plugin" (wired in-process via wireInPlugin) ──
// A grandparent-scale-aware crumble: it responds ONLY to events at the coarsest
// (macro) scale and clears the unstable macro's full 1 m grid footprint through the
// public edit path (ctx->apply_edit). Finer-scale events are tallied for the HUD
// but never actioned — see the file header for why responding at every scale would
// self-destruct the deck. This is tests/MultiLevelPropagationTest.cpp's macroCrumble,
// productized; the g_* statics mirror that test's g_resp/g_events pattern (a plugin
// init takes no user_data, so its state is file-scope).
PluginContext* g_resp = nullptr;
long long g_macroCollapses = 0;  // macro-scale (grandparent) events actioned
long long g_microEvents    = 0;  // micro-scale events detected, deliberately not actioned
long long g_otherEvents    = 0;  // any other scale (defensive; expected 0)

constexpr double    kGridSizeM     = 1.0;  // terminal (editable) voxel size
constexpr long long kMacroScaleInt = 4;    // llround(macro voxel size) — the coarsest scale

void grandparentCrumble(const StructuralEvent* ev, void* /*user_data*/) {
    if (!ev) return;
    const long long scale = std::llround(ev->voxel_size_m);
    if (scale != kMacroScaleInt) {           // not the grandparent scale → count only
        if (scale == 2) ++g_microEvents;
        else            ++g_otherEvents;
        return;
    }
    ++g_macroCollapses;
    if (!g_resp) return;

    // Clear every 1 m grid voxel under the unstable 4 m macro. Each apply_edit takes
    // the single edit choke point, so each cleared grandchild re-dirties the chain;
    // the engine re-aggregates and re-floods next frame and finds the next ring of
    // unstable macros — the cave-in cascade, now driven at the grandparent scale.
    const double half  = ev->voxel_size_m * 0.5;
    const double ghalf = kGridSizeM * 0.5;
    const int    n     = static_cast<int>(std::llround(ev->voxel_size_m / kGridSizeM));
    const Voxel  empty = Voxel::empty();
    for (int dz = 0; dz < n; ++dz)
        for (int dy = 0; dy < n; ++dy)
            for (int dx = 0; dx < n; ++dx)
                g_resp->apply_edit(
                    g_resp,
                    WorldCoord(ev->position.value.x - half + ghalf + dx * kGridSizeM,
                               ev->position.value.y - half + ghalf + dy * kGridSizeM,
                               ev->position.value.z - half + ghalf + dz * kGridSizeM),
                    &empty);
}

int grandparentCrumbleInit(PluginContext* ctx) {
    g_resp = ctx;
    ctx->register_on_structural_event(ctx, grandparentCrumble, nullptr);
    return 0;
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
    Layer* macro   = world.layer("macro");
    Layer* micro   = world.layer("micro");
    Layer* grid    = world.layer("grid");
    Layer* bedrock_l = world.layer("bedrock");
    if (!macro || !micro || !grid || !bedrock_l) {
        Log::error(kLogCat, "Fatal: expected macro/micro/grid/bedrock layers.");
        return 1;
    }

    const double mvs = macro->voxelSizeM();      // grandparent voxel size, 4 m
    const double gvs = grid->voxelSizeM();        // terminal grid voxel size, 1 m
    const double bvs = bedrock_l->voxelSizeM();    // bedrock voxel size, 0.5 m

    // The macro chunk is loaded EMPTY so every macro voxel reads as decomposed
    // (resident, no block voxel) — its aggregate then comes from its micro/grid
    // descendants, which is what mining changes (§7). One 64 m chunk spans the
    // whole scene, so deck-perpendicular macro neighbors are resident-empty.
    macro->loadChunk(ChunkCoord{0, 0, 0}, nullptr);

    // Stream in the micro + grid chunks covering one macro's footprint so its
    // aggregate can be computed and its grid voxels edited. One macro = one micro
    // chunk (4 m) = a 2×2×2 block of grid chunks (2 m each). Mirrors the test's
    // ensureResident.
    auto ensureResident = [&](VoxelCoord m) {
        micro->loadChunk(ChunkCoord{static_cast<int32_t>(m.x),
                                    static_cast<int32_t>(m.y),
                                    static_cast<int32_t>(m.z)}, nullptr);
        for (int az = 0; az < 2; ++az)
            for (int ay = 0; ay < 2; ++ay)
                for (int ax = 0; ax < 2; ++ax)
                    grid->loadChunk(ChunkCoord{static_cast<int32_t>(2 * m.x + ax),
                                               static_cast<int32_t>(2 * m.y + ay),
                                               static_cast<int32_t>(2 * m.z + az)},
                                    nullptr);
    };

    // Fill all kMacroToGrid³ grid children of a macro with the given voxel, by
    // DIRECT setVoxel (bypassing the edit choke point so construction fires no
    // structural events — the deck must build silently, like demo 13).
    auto fillMacroGrid = [&](VoxelCoord m, const Voxel& v) {
        const VoxelCoord gmin = chunkmath::childVoxelMin(m, kMacroToGrid);
        for (int64_t dz = 0; dz < kMacroToGrid; ++dz)
            for (int64_t dy = 0; dy < kMacroToGrid; ++dy)
                for (int64_t dx = 0; dx < kMacroToGrid; ++dx)
                    grid->setVoxel(
                        chunkmath::voxelCenter({gmin.x + dx, gmin.y + dy, gmin.z + dz}, gvs),
                        v);
    };

    // Fill a macro's whole volume with bedrock voxels (mvs/bvs = 8 per edge), so the
    // immutable layer is solid across the macro and isAnchor() samples it at the
    // macro/micro center (PropagationSystem::isAnchor).
    const int bcs = bedrock_l->chunkSizeVoxels();
    auto fillMacroBedrock = [&](VoxelCoord m) {
        const int64_t r = chunkmath::layerRatio(mvs, bvs);  // bedrock voxels per macro edge
        const VoxelCoord bmin{m.x * r, m.y * r, m.z * r};
        for (int64_t dz = 0; dz < r; ++dz)
            for (int64_t dy = 0; dy < r; ++dy)
                for (int64_t dx = 0; dx < r; ++dx) {
                    const VoxelCoord bv{bmin.x + dx, bmin.y + dy, bmin.z + dz};
                    // The scene spans several bedrock chunks; setVoxel no-ops on an
                    // unloaded chunk, so load it on demand (loadChunk is idempotent).
                    bedrock_l->loadChunk(chunkmath::voxelToChunkLocal(bv, bcs).chunk, nullptr);
                    bedrock_l->setVoxel(chunkmath::voxelCenter(bv, bvs), bedrock());
                }
    };

    // Build the immutable bedrock: a floor slab plus the two anchor towers. Done
    // once — bedrock never changes (it is the boundary the cascade stops at).
    for (int64_t x = kXLTower; x <= kXRTower; ++x) fillMacroBedrock({x, kYFloor, kZ});  // floor
    for (int64_t y = kYFloor; y <= kYDeck; ++y) {
        fillMacroBedrock({kXLTower, y, kZ});  // left tower
        fillMacroBedrock({kXRTower, y, kZ});  // right tower
    }

    // (Re)build the stone deck between the tower tops. Also used by the R reset.
    auto buildDeck = [&]() {
        for (int64_t x = kDeckX0; x <= kDeckX1; ++x) {
            ensureResident({x, kYDeck, kZ});
            fillMacroGrid({x, kYDeck, kZ}, stone());
        }
    };
    // Clear any stone the cascade may have left in the deck row, for the R reset.
    auto clearDeck = [&]() {
        for (int64_t x = kDeckX0; x <= kDeckX1; ++x)
            fillMacroGrid({x, kYDeck, kZ}, Voxel::empty());
    };
    buildDeck();

    // ── Plugin / edit plumbing ─────────────────────────────────────────────────
    PluginManager pm;
    Engine engine;
    engine.init(pm, world);

    // The edit choke point: routes both player edits and the response's apply_edit
    // through World::setVoxel + on_voxel_modified. PhysicsSystem rides that hook to
    // observe every edit (§7, §15).
    net::NetworkManager nm;
    nm.init(world, pm);

    // The demo's grandparent-scale crumble response (in-process plugin).
    pm.wireInPlugin(grandparentCrumbleInit);

    // The driver. Constructed after init so its engine-owned on_voxel_modified hook
    // is in place for every subsequent edit. unique_ptr so R can reset its cascade
    // bookkeeping by rebuilding it.
    auto physics = std::make_unique<sim::PhysicsSystem>(world, pm);

    // ── Edit tracking → remesh ─────────────────────────────────────────────────
    // A host-owned on_voxel_modified hook records which grid chunks any edit touched
    // (player OR response), so we remesh exactly those once per frame.
    struct EditTracker {
        std::unordered_set<ChunkCoord, ChunkCoordHash> touched;
        int    chunkSize = 2;
        double voxelSize = 1.0;
    } tracker;
    tracker.chunkSize = grid->chunkSizeVoxels();
    tracker.voxelSize = gvs;
    pm.registerEngineVoxelModifiedHook(
        [](WorldCoord pos, const Voxel*, const Voxel*, PlayerId, void* ud) {
            auto* t = static_cast<EditTracker*>(ud);
            const VoxelCoord v = chunkmath::worldToVoxel(pos, t->voxelSize);
            t->touched.insert(chunkmath::voxelToChunkLocal(v, t->chunkSize).chunk);
        },
        &tracker);

    // ── Renderer ───────────────────────────────────────────────────────────────
    platform::Window window(1280, 720, "VoxelEngine — M17 Multi-Level Collapse");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setFarClip(400.0f);
    renderer.setCrosshair(true);

    // Bedrock never changes → build its mesh once. Grid meshes are rebuilt on edit
    // from the tracker. (The macro/micro layers are empty, so they have no mesh.)
    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> gridMeshes;
    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> bedrockMeshes;
    auto rebuildGridChunk = [&](const ChunkCoord& cc) {
        auto it = gridMeshes.find(cc);
        if (it != gridMeshes.end()) { it->second.destroy(); gridMeshes.erase(it); }
        const Chunk* ch = grid->getChunk(cc);
        if (!ch) return;
        ChunkMesh m = ChunkMesh::build(*ch);
        if (!m.empty()) gridMeshes.emplace(cc, std::move(m));
    };
    auto rebuildAllGrid = [&]() {
        for (auto& kv : gridMeshes) kv.second.destroy();
        gridMeshes.clear();
        for (const auto& kv : grid->chunks()) rebuildGridChunk(kv.first);
    };
    rebuildAllGrid();
    for (const auto& kv : bedrock_l->chunks()) {
        ChunkMesh m = ChunkMesh::build(*kv.second);
        if (!m.empty()) bedrockMeshes.emplace(kv.first, std::move(m));
    }

    // ── Camera (fly) ───────────────────────────────────────────────────────────
    // Start in front of the bridge looking -Z so the single-thick scene reads as a
    // clean 2D side view. Deck spans x 16..48 m, y 24..28 m, z 32..36 m.
    float      pitch = -0.12f, yaw = 3.14159265f;  // yaw=π → forward points -Z
    WorldCoord camPos(32.0, 27.0, 56.0);
    double     lastMX = 0, lastMY = 0;
    bool       firstMouse = true, cursorCaptured = true;
    bool       prevF = false, prevR = false, prevRight = false;

    sim::RemovalAccumulator remover;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    auto prevTime = std::chrono::high_resolution_clock::now();

    Log::info(kLogCat,
        "Multi-level collapse — fly to the stone bridge and HOLD LEFT-CLICK to mine the "
        "1 m grid voxels (grandchildren) of a deck macro near a tower; the 4 m macro "
        "grandparent's aggregate falls until it caves, and the cascade stops at the "
        "macros still anchored to the far tower. RIGHT-CLICK knocks out a whole macro at "
        "once. Keys: R rebuild, F cursor, ESC quit.");

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

        // R: rebuild the bridge and reset the cascade state + counters.
        const bool curR = (glfwGetKey(glfwWin, GLFW_KEY_R) == GLFW_PRESS);
        if (curR && !prevR) {
            clearDeck();
            buildDeck();
            physics.reset();                                   // drop old cascade bookkeeping
            physics = std::make_unique<sim::PhysicsSystem>(world, pm);
            g_macroCollapses = g_microEvents = g_otherEvents = 0;
            remover.reset();
            rebuildAllGrid();
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

        // ── Mining ──────────────────────────────────────────────────────────────
        // Raycast picks only the GRID (primary terminal) layer, so the player can
        // never target the immutable bedrock anchors. Every removal routes through
        // the edit choke point so PropagationSystem sees the grandchild edit and
        // walks it up grid→micro→macro.
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, look, kReachM);

        // HOLD LEFT: chew the targeted 1 m grid voxel (gradual hollow). The macro's
        // aggregate falls one grid voxel at a time — the upward propagation made
        // continuous.
        const bool curLeft = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        if (hit.hit && curLeft) {
            const Voxel target = world.getVoxel(chunkmath::voxelCenter(hit.voxel, gvs));
            if (remover.accrue(hit.voxel, target.material.hardness, kToolPower, dt)) {
                nm.applyEdit(kLocalPlayer, chunkmath::voxelCenter(hit.voxel, gvs), Voxel::empty());
                remover.reset();
            }
        } else {
            remover.reset();
        }

        // RIGHT (edge): knock out the WHOLE targeted 4 m macro at once — all 64 grid
        // grandchildren in one frame, the fastest way to sever a support and trigger
        // the grandparent cascade.
        const bool curRight = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
        if (hit.hit && curRight && !prevRight) {
            const VoxelCoord m    = chunkmath::childToParentVoxel(hit.voxel, kMacroToGrid);
            const VoxelCoord gmin = chunkmath::childVoxelMin(m, kMacroToGrid);
            const Voxel empty = Voxel::empty();
            for (int64_t dz = 0; dz < kMacroToGrid; ++dz)
                for (int64_t dy = 0; dy < kMacroToGrid; ++dy)
                    for (int64_t dx = 0; dx < kMacroToGrid; ++dx)
                        nm.applyEdit(kLocalPlayer,
                                     chunkmath::voxelCenter(
                                         {gmin.x + dx, gmin.y + dy, gmin.z + dz}, gvs),
                                     empty);
        }
        prevRight = curRight;

        // ── End-of-frame structural pass ───────────────────────────────────────
        // Drains dirty macros fine→coarse, recomputes aggregates up the chain, runs
        // the support flood at every level, and fires on_structural_event for newly-
        // unstable macros within budget. The response's edits land through the choke
        // point above and re-dirty for the next frame — the cascade feedback loop.
        physics->tick();

        // Remesh exactly the grid chunks any edit (player or response) touched.
        for (const ChunkCoord& cc : tracker.touched) rebuildGridChunk(cc);
        tracker.touched.clear();

        // ── HUD ────────────────────────────────────────────────────────────────
        char line0[200];
        std::snprintf(line0, sizeof(line0),
                      "Upward chain:  grandparent (macro) collapses: %lld   |   micro-scale "
                      "events detected (not actioned): %lld",
                      g_macroCollapses, g_microEvents);
        char line1[200];
        std::snprintf(line1, sizeof(line1),
                      "This frame: %d structural events   carried backlog: %zu   |   HOLD LMB "
                      "mine grid voxel  RMB knock out macro  R rebuild",
                      physics->eventsFiredLastTick(), physics->carryBacklog());
        renderer.setHudText({std::string(line0), std::string(line1)});

        if (hit.hit) {
            const float progress =
                (remover.hasTarget() && remover.target() == hit.voxel)
                    ? remover.progress() : -1.0f;
            renderer.drawVoxelHighlight(chunkmath::voxelCenter(hit.voxel, gvs),
                                        static_cast<float>(gvs), 0xff00ffff, progress);
        }

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
        for (const auto& kv : gridMeshes) {
            const Chunk* ch = grid->getChunk(kv.first);
            if (ch) renderer.renderChunk(kv.second, ch->origin(), gvs, grid->chunkSizeVoxels());
        }
        renderer.render();
    }

    // Teardown. Drop the physics driver (unregisters its engine hook) and the host's
    // remesh tracker before the plugin manager so no torn-down callback is left
    // dangling.
    physics.reset();
    pm.unregisterEngineVoxelModifiedHook(&tracker);
    for (auto& kv : gridMeshes) kv.second.destroy();
    for (auto& kv : bedrockMeshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

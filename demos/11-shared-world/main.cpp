// M11 demo — shared world.
//
// A two-player session on localhost that exercises every M11 system. The host
// runs the authority in-process (host-as-authority P2P): launch it with
//
//     ./11-shared-world --host [port]
//
// and join from a second terminal (or machine) with
//
//     ./11-shared-world --join <host-address> [port]
//
// Both sides load the server-authority plugin, so the same last-write-wins
// authority model runs whether this process is the host or a client. The world
// is the layered-world composite-over-terminal stack (8 m composite "blocks"
// decomposing into 1 m terminal "terrain" above an immutable "backdrop" slab),
// derived identically on both sides — only player edits and player positions
// ever cross the wire:
//
//   - Edit replication: every break/place goes through the single
//     NetworkManager::applyEdit choke point. On the host it commits and
//     broadcasts; on a client it is forwarded as an EditIntent and applied when
//     the committed edit returns — one round trip. The HUD prints the `source`
//     of the last on_voxel_modified event (local or the remote player's id).
//   - Decomposition: each side decomposes composite blocks on approach,
//     locally and deterministically. No decomposition data crosses the wire —
//     watch the HUD packet-rate counter stay flat while terrain pops in.
//   - Join handshake: a joining client receives the world seed + LayerConfig
//     and the host's dirty (player-edited) chunks, nothing else.
//   - Chat: press T, type, Enter to send on the "engine.chat" channel
//     (Reliable + Broadcast, relayed by the authority); Escape dismisses.
//   - Interest management (host only): press I to cycle broadcast-all →
//     mirrored-streaming-radius → plugin filter (a wired-in demo filter that
//     suppresses edits for peers more than a fixed distance away).
//   - Player presence: positions travel the "engine.player_position" channel
//     (Unreliable + Broadcast) every N frames; remote players render as
//     colored marker cubes, with per-peer RTT in the HUD.
//
// Known simplifications (deliberate, single-screen demo scope): a client edit
// in a region the host has not decomposed commits into thin air on the host
// (last-known-position markers, no interpolation), and a client that evicts a
// dirty terrain chunk re-derives the clean version until it rejoins — the
// authoritative save lives with the host.
//
// Controls: WASD move, mouse look, Space/Shift fly up/down (or Space jump in
// walk mode), G toggle walk/fly, left/right mouse break/place, 1-9 select
// material, T chat, I cycle interest mode (host), F toggle cursor, ESC quits
// (or closes the chat line).

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "io/ChunkPersistence.h"
#include "net/NetworkManager.h"
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef VOXEL_LAYERED_PLUGIN_PATH
#  define VOXEL_LAYERED_PLUGIN_PATH ""
#endif
#ifndef VOXEL_AUTHORITY_PLUGIN_PATH
#  define VOXEL_AUTHORITY_PLUGIN_PATH ""
#endif
#ifndef VOXEL_CHAT_PLUGIN_PATH
#  define VOXEL_CHAT_PLUGIN_PATH ""
#endif

namespace {

constexpr char kLogCat[] = "demo11";

constexpr uint16_t kDefaultPort       = 27777;
constexpr uint64_t kWorldSeed         = 1337;   // shared via the join handshake
constexpr int      kStreamPerFrame    = 2;
constexpr int      kDecomposePerFrame = 48;
constexpr double   kDecomposeRadiusM  = 144.0;
constexpr double   kTerrainKeepRadiusM = 160.0; // see 05-decompose-on-approach
constexpr int      kPositionEveryNFrames = 10;  // position-replication cadence
constexpr double   kSaveIntervalS     = 5.0;    // host autosave cadence
constexpr double   kFilterDistanceM   = 48.0;   // demo interest-filter cutoff
constexpr float    kFlySpeed          = 32.0f;
constexpr float    kMouseSens         = 0.002f;
constexpr double   kReachM            = 8.0;

constexpr double kWalkSpeed = 6.0;
constexpr double kGravity   = 25.0;
constexpr double kJumpSpeed = 8.0;
constexpr double kEyeOffset = 0.7;
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

using MeshStore = std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash>;

// ── Demo plugin state (captureless hooks wired in via wireInPlugin) ──────────

struct PendingEdit {
    WorldCoord pos;
    PlayerId   source;
};
std::vector<PendingEdit>  g_pendingEdits;  // drained by the main loop (remesh + HUD)
std::vector<std::string>  g_chatLog;       // chat + join/leave lines for the HUD
net::NetworkManager*      g_net = nullptr; // for the distance interest filter

void appendChat(const std::string& line) {
    g_chatLog.push_back(line);
    if (g_chatLog.size() > 6) g_chatLog.erase(g_chatLog.begin());
}

void onVoxelModified(WorldCoord pos, const Voxel*, const Voxel*,
                     PlayerId source, void*) {
    g_pendingEdits.push_back({pos, source});
}

void onChatMessage(const MessageEnvelope* env, void*) {
    if (!env || !env->payload || env->payload_size == 0) return;
    const std::string text(static_cast<const char*>(env->payload), env->payload_size);
    appendChat("player " + std::to_string(env->sender_id) + ": " + text);
}

void onPlayerJoined(PlayerId id, WorldCoord, void*) {
    appendChat("* player " + std::to_string(id) + " joined");
}
void onPlayerLeft(PlayerId id, void*) {
    appendChat("* player " + std::to_string(id) + " left");
}

int demoHooksInit(PluginContext* ctx) {
    ctx->register_on_voxel_modified(ctx, onVoxelModified, nullptr);
    ctx->register_on_network_message(ctx, "engine.chat", onChatMessage, nullptr);
    ctx->register_on_player_joined(ctx, onPlayerJoined, nullptr);
    ctx->register_on_player_left(ctx, onPlayerLeft, nullptr);
    return 0;
}

// Demo interest filter: suppress an edit for any peer whose last known position
// is more than kFilterDistanceM from it. Loaded/unloaded by the I-key cycle —
// while registered it overrides the built-in interest mode entirely.
bool distanceFilter(PlayerId peer, WorldCoord edit_pos, void*) {
    if (!g_net) return true;
    const auto& positions = g_net->playerPositions();
    auto it = positions.find(peer);
    if (it == positions.end()) return true;  // unknown position → deliver
    return glm::length(edit_pos.value - it->second.value) <= kFilterDistanceM;
}
int filterPluginInit(PluginContext* ctx) {
    ctx->register_interest_filter(ctx, distanceFilter, nullptr);
    return 0;
}

// ── Chat input line (GLFW char callback) ─────────────────────────────────────

// The 't' that opens the line never lands in it: its char event fires during
// the same pollEvents call that updates the key state, i.e. before the key
// poll below activates the input line.
bool        g_chatInputActive = false;
std::string g_chatInput;

void charCallback(GLFWwindow*, unsigned int codepoint) {
    if (!g_chatInputActive) return;
    if (codepoint >= 32 && codepoint < 127 && g_chatInput.size() < 96)
        g_chatInput.push_back(static_cast<char>(codepoint));
}

// The child terrain chunks covering one composite macro voxel's subvolume
// (same shape as 05-decompose-on-approach).
std::vector<ChunkCoord> childChunksForMacro(const chunkmath::VoxelCoord& macro,
                                            const Layer& parent, const Layer& child) {
    const double parentVoxel    = parent.voxelSizeM();
    const double childChunkSize = child.voxelSizeM() * child.chunkSizeVoxels();
    const int    span = std::max(1, static_cast<int>(std::llround(parentVoxel / childChunkSize)));
    const WorldCoord origin = chunkmath::voxelOrigin(macro, parentVoxel);
    const ChunkCoord base =
        chunkmath::worldToChunk(origin, child.voxelSizeM(), child.chunkSizeVoxels());

    std::vector<ChunkCoord> out;
    out.reserve(static_cast<size_t>(span) * span * span);
    for (int dz = 0; dz < span; ++dz)
        for (int dy = 0; dy < span; ++dy)
            for (int dx = 0; dx < span; ++dx)
                out.push_back(ChunkCoord{base.x + dx, base.y + dy, base.z + dz});
    return out;
}

// Marker colors for remote players (ABGR), indexed by player id.
constexpr uint32_t kPlayerColors[] = {0xff0000ff, 0xff00ff00, 0xffff8800, 0xffff00ff};

}  // namespace

int main(int argc, char** argv) {
    // ── CLI: --host [port] | --join <address> [port] ──────────────────────
    bool        isHost = false;
    std::string joinAddress;
    uint16_t    port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host") {
            isHost = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--join" && i + 1 < argc) {
            joinAddress = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-')
                port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
    }
    if (!isHost && joinAddress.empty()) {
        Log::error(kLogCat, "Usage: 11-shared-world --host [port] | "
                            "--join <host-address> [port]");
        return 1;
    }

    LayerConfig layerConfig = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 8
    view_distance_chunks: 4
  - name: backdrop
    voxel_size_m: 2.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 6
    resident_chunk_budget: 512
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    view_distance_chunks: 6
)");
        } catch (const std::exception& e) {
            Log::error(kLogCat, (std::string("Fatal: layer config error: ") + e.what()).c_str());
            std::exit(1);
        }
    }();

    // ── Plugins: world generators + authority model + chat ────────────────
    PluginManager pluginManager;
    for (const char* path : {VOXEL_LAYERED_PLUGIN_PATH, VOXEL_AUTHORITY_PLUGIN_PATH,
                             VOXEL_CHAT_PLUGIN_PATH}) {
        if (std::strlen(path) == 0 ||
            pluginManager.loadPlugin(path) == kInvalidPluginId) {
            Log::error(kLogCat, (std::string("Fatal: could not load plugin '") + path + "'").c_str());
            return 1;
        }
    }
    pluginManager.wireInPlugin(demoHooksInit);

    auto findGenerator = [&](const std::string& name) -> RegisteredLayerGenerator {
        for (const auto& g : pluginManager.layerGenerators())
            if (g.layer_name == name) return g;
        return RegisteredLayerGenerator{name, nullptr, nullptr, kInvalidPluginId};
    };
    const RegisteredLayerGenerator blocksGen   = findGenerator("blocks");
    const RegisteredLayerGenerator backdropGen = findGenerator("backdrop");
    const RegisteredLayerGenerator terrainGen  = findGenerator("terrain");
    if (!blocksGen.fn || !backdropGen.fn || !terrainGen.fn) {
        Log::error(kLogCat, "Fatal: layered-world plugin did not register all three "
                            "layer generators (blocks, backdrop, terrain).");
        return 1;
    }

    std::vector<std::string> buildMaterials;
    for (size_t i = 0; i < pluginManager.materials().size() && i < 9; ++i)
        buildMaterials.push_back(pluginManager.materials()[i].material_id);
    if (buildMaterials.empty()) {
        Log::error(kLogCat, "Fatal: no materials registered by the plugin.");
        return 1;
    }
    size_t selectedMaterial = 0;

    // ── World, persistence, networking ─────────────────────────────────────
    World world(layerConfig);
    Layer* blocks   = world.layer("blocks");
    Layer* backdrop = world.layer("backdrop");
    Layer* terrain  = world.layer("terrain");
    if (!blocks || !backdrop || !terrain) {
        Log::error(kLogCat, "Fatal: expected blocks/backdrop/terrain layers.");
        return 1;
    }

    // The host persists dirty terrain chunks — both so edits survive relaunch
    // and so the join handshake can stream them to late joiners.
    persistence::WorldSave save("shared-world-save",
        persistence::WorldIdentity{terrain->voxelSizeM(), terrain->chunkSizeVoxels()});

    net::NetworkManager nm;
    g_net = &nm;
    nm.init(world, pluginManager);
    if (isHost) {
        nm.setWorldSave(&save);
        nm.setWorldSeed(kWorldSeed);
        nm.setLayerConfig(&layerConfig);
    }
    // The actual listen/connect happens after the renderer is up (below): the
    // connect handshake is serviced by nm.update() in the main loop, and a slow
    // GPU init in between would let the attempt time out before the first poll.

    Engine engine;
    engine.start();
    // Note: the NetworkManager is deliberately NOT attached to the Engine here.
    // Engine::tick() would pump it from the engine's own game-loop thread, while
    // this demo (like every demo) owns its frame loop and services nm.update()
    // there — world access and meshing must stay on this one thread.

    platform::Window window(1024, 768,
        isHost ? "VoxelEngine — M11 Shared World (host)"
               : "VoxelEngine — M11 Shared World (client)");

    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(true);

    LODManager lod(layerConfig);
    lod.setVerticalBand(-1, 0);

    // The immutable backdrop's per-layer chunk cap (M16 L5): shed farthest-first
    // when the resident set exceeds it. Immutable chunks regenerate from seed, so
    // shedding is free.
    const int backdropBudget = layerConfig.findLayer("backdrop")->resident_chunk_budget;

    // NOTE: unlike demos 05/09/10, this demo deliberately keeps its composite
    // decomposition FRONT-END-OWNED (DecompositionWorker below) rather than handing
    // it to the engine's DecompositionManager. The manager's decompose-apply inserts
    // freshly generated child chunks, overwriting any already-resident terrain — but
    // here a terrain chunk may already hold replicated player edits (from the join
    // handshake, the live edit stream into a not-yet-decomposed region, or the host's
    // save) that determinism cannot re-derive. Preserving those resident edits across
    // decomposition is exactly what this networking demo teaches, so the drain loop
    // below keeps resident/saved chunks and only inserts generated terrain where none
    // exists. (A manager substitution hook could close this gap — see the demo-pass
    // notes — but that is engine surface beyond this teaching refactor.)
    DecompositionState  decomp;
    DecompositionWorker worker;

    Voxel blockTemplate;
    bool  haveBlockTemplate = false;

    MeshStore blocksMeshes, backdropMeshes, terrainMeshes;

    // (Re)build the GPU mesh of the terrain chunk owning an edited voxel.
    auto remeshTerrainAt = [&](const WorldCoord& pos) {
        const chunkmath::VoxelCoord vc =
            chunkmath::worldToVoxel(pos, terrain->voxelSizeM());
        const ChunkCoord cc =
            chunkmath::voxelToChunkLocal(vc, terrain->chunkSizeVoxels()).chunk;
        const Chunk* chunk = terrain->getChunk(cc);
        if (!chunk) return;
        auto it = terrainMeshes.find(cc);
        if (it != terrainMeshes.end()) {
            it->second.destroy();
            it->second = ChunkMesh::build(*chunk);
        } else {
            terrainMeshes.emplace(cc, ChunkMesh::build(*chunk));
        }
    };

    // Camera / player state.
    float      pitch = -0.35f, yaw = 0.0f;
    WorldCoord camPos(0.0, 40.0, -48.0);
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true;
    bool       cursorCaptured = true;
    bool       prevKeyF = false, prevKeyG = false, prevKeyT = false,
               prevKeyI = false, prevKeyEnter = false, prevKeyEsc = false,
               prevKeyBackspace = false;
    bool       prevLeft = false, prevRight = false;

    bool       walkMode = false;
    WorldCoord playerCenter(0.0, 0.0, 0.0);
    double     vy = 0.0;
    bool       grounded = false;

    // Interest-mode cycle state (host side).
    enum class InterestUi { BroadcastAll, StreamingRadius, PluginFilter };
    InterestUi interestUi   = InterestUi::BroadcastAll;
    PluginId   filterPlugin = kInvalidPluginId;

    // HUD bookkeeping.
    std::string lastEditSource = "-";
    uint64_t    lastPacketCount = 0;
    double      packetTimer = 0.0, packetRate = 0.0;
    double      saveTimer = 0.0;
    int         frameCounter = 0;

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCharCallback(glfwWin, charCallback);

    // Go live on the network now that the frame loop is about to service it.
    if (isHost) {
        if (!nm.startHostPeer(port)) {
            Log::error(kLogCat, (std::string("Fatal: could not bind port ")
                                 + std::to_string(port)).c_str());
            return 1;
        }
        Log::info(kLogCat, (std::string("Hosting on port ") + std::to_string(port)
                            + " (host-as-authority P2P). Save: " + save.directory()).c_str());
    } else {
        if (!nm.startClient(joinAddress, port)) {
            Log::error(kLogCat, (std::string("Fatal: could not connect to ") + joinAddress
                                 + ":" + std::to_string(port)).c_str());
            return 1;
        }
        Log::info(kLogCat, (std::string("Joining ") + joinAddress + ":"
                            + std::to_string(port) + " ...").c_str());
    }

    auto prevTime = std::chrono::high_resolution_clock::now();

    Log::info(kLogCat, "WASD + mouse, Space/Shift up/down, G walk, left/right mouse "
                       "break/place, 1-9 material, T chat, I interest mode (host), "
                       "F cursor, ESC quits.");

    bool quit = false;
    while (!window.shouldClose() && !quit) {
        window.pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        // ── Chat input line ────────────────────────────────────────────────
        bool curKeyT     = (glfwGetKey(glfwWin, GLFW_KEY_T)         == GLFW_PRESS);
        bool curKeyEnter = (glfwGetKey(glfwWin, GLFW_KEY_ENTER)     == GLFW_PRESS);
        bool curKeyEsc   = (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE)    == GLFW_PRESS);
        bool curKeyBs    = (glfwGetKey(glfwWin, GLFW_KEY_BACKSPACE) == GLFW_PRESS);

        if (!g_chatInputActive) {
            if (curKeyEsc && !prevKeyEsc) quit = true;
            if (curKeyT && !prevKeyT) {
                g_chatInputActive = true;
                g_chatInput.clear();
            }
        } else {
            if (curKeyEsc && !prevKeyEsc) {
                g_chatInputActive = false;  // dismiss without sending
            } else if (curKeyEnter && !prevKeyEnter) {
                if (!g_chatInput.empty()) {
                    MessageEnvelope env{};
                    env.channel_id   = "engine.chat";
                    env.target       = MessageTarget::Broadcast;
                    env.reliability  = MessageReliability::Reliable;
                    env.payload      = g_chatInput.c_str();
                    env.payload_size = g_chatInput.size();
                    nm.sendNetworkMessage(env);  // engine deep-copies the payload
                    appendChat("me: " + g_chatInput);
                }
                g_chatInputActive = false;
            } else if (curKeyBs && !prevKeyBackspace && !g_chatInput.empty()) {
                g_chatInput.pop_back();
            }
        }
        prevKeyT         = curKeyT;
        prevKeyEnter     = curKeyEnter;
        prevKeyEsc       = curKeyEsc;
        prevKeyBackspace = curKeyBs;

        bool curKeyF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curKeyF && !prevKeyF && !g_chatInputActive) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                             cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevKeyF = curKeyF;

        bool curKeyG = (glfwGetKey(glfwWin, GLFW_KEY_G) == GLFW_PRESS);
        if (curKeyG && !prevKeyG && !g_chatInputActive) {
            walkMode = !walkMode;
            if (walkMode) {
                playerCenter = WorldCoord(camPos.value - glm::dvec3(0.0, kEyeOffset, 0.0));
                vy = 0.0;
                grounded = false;
            }
        }
        prevKeyG = curKeyG;

        // I — cycle interest mode (authority only; a client shows a hint).
        bool curKeyI = (glfwGetKey(glfwWin, GLFW_KEY_I) == GLFW_PRESS);
        if (curKeyI && !prevKeyI && !g_chatInputActive) {
            if (!isHost) {
                appendChat("* interest mode is chosen at the host");
            } else {
                switch (interestUi) {
                    case InterestUi::BroadcastAll:
                        interestUi = InterestUi::StreamingRadius;
                        nm.setInterestMode(net::NetworkManager::InterestMode::StreamingRadius);
                        break;
                    case InterestUi::StreamingRadius:
                        interestUi   = InterestUi::PluginFilter;
                        filterPlugin = pluginManager.wireInPlugin(filterPluginInit);
                        break;
                    case InterestUi::PluginFilter:
                        interestUi = InterestUi::BroadcastAll;
                        if (filterPlugin != kInvalidPluginId) {
                            pluginManager.unloadPlugin(filterPlugin);
                            filterPlugin = kInvalidPluginId;
                        }
                        nm.setInterestMode(net::NetworkManager::InterestMode::BroadcastAll);
                        break;
                }
            }
        }
        prevKeyI = curKeyI;

        if (!g_chatInputActive) {
            for (int i = 0; i < static_cast<int>(buildMaterials.size()); ++i)
                if (glfwGetKey(glfwWin, GLFW_KEY_1 + i) == GLFW_PRESS)
                    selectedMaterial = static_cast<size_t>(i);
        }

        // ── Mouse look / movement (frozen while typing) ───────────────────
        if (cursorCaptured) {
            double mx, my;
            glfwGetCursorPos(glfwWin, &mx, &my);
            if (!firstMouse && !g_chatInputActive) {
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

        if (!g_chatInputActive) {
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
                if (glm::length(wish) > 0.0) wish = glm::normalize(wish);

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
            }
        }

        // ── Network tick: poll, dispatch, flush ────────────────────────────
        nm.update(dt);

        // Replicate our position every N frames (Unreliable + Broadcast).
        if (++frameCounter % kPositionEveryNFrames == 0)
            nm.broadcastLocalPosition(camPos);

        // Drain edits committed this tick — local or replicated — and remesh.
        for (const PendingEdit& e : g_pendingEdits) {
            remeshTerrainAt(e.pos);
            lastEditSource = (e.source == kLocalPlayer)
                                 ? "local"
                                 : ("player " + std::to_string(e.source));
        }
        g_pendingEdits.clear();

        // ── Stream the composite and immutable layers around the camera ────
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
            const bool composite = (layer == blocks);
            std::vector<ChunkCoord> toEvict;
            for (const auto& kv : meshes)
                if (lod.shouldEvict(center, kv.first, name)) toEvict.push_back(kv.first);
            for (const ChunkCoord& c : toEvict) {
                if (composite) {
                    const int n = layer->chunkSizeVoxels();
                    bool anyPending = false;
                    for (int z = 0; z < n && !anyPending; ++z)
                        for (int y = 0; y < n && !anyPending; ++y)
                            for (int x = 0; x < n && !anyPending; ++x)
                                if (decomp.isPending(chunkmath::chunkLocalToVoxel(c, x, y, z, n)))
                                    anyPending = true;
                    if (anyPending) continue;
                    for (int z = 0; z < n; ++z)
                        for (int y = 0; y < n; ++y)
                            for (int x = 0; x < n; ++x) {
                                const chunkmath::VoxelCoord V =
                                    chunkmath::chunkLocalToVoxel(c, x, y, z, n);
                                if (!decomp.isDecomposed(V)) continue;
                                for (const ChunkCoord& tcc :
                                     childChunksForMacro(V, *blocks, *terrain)) {
                                    if (isHost && terrain->isChunkDirty(tcc)) {
                                        if (const Chunk* ch = terrain->getChunk(tcc))
                                            save.saveChunk(*ch);
                                        terrain->clearChunkDirty(tcc);
                                    }
                                    auto it = terrainMeshes.find(tcc);
                                    if (it != terrainMeshes.end()) {
                                        it->second.destroy();
                                        terrainMeshes.erase(it);
                                    }
                                    terrain->unloadChunk(tcc);
                                }
                                decomp.clear(V);
                            }
                }
                meshes[c].destroy();
                meshes.erase(c);
                layer->unloadChunk(c);
            }
        };
        stream(blocks,   "blocks",   blocksMeshes,   blocksGen);
        stream(backdrop, "backdrop", backdropMeshes, backdropGen);

        // Enforce the immutable backdrop's resident_chunk_budget (M16 L5): if the
        // resident set still exceeds the cap, shed farthest-first. Immutable chunks
        // regenerate from seed, so this is free (no dirty/persist path).
        if (backdropBudget > 0 &&
            static_cast<int>(backdropMeshes.size()) > backdropBudget) {
            const ChunkCoord bc = chunkmath::worldToChunk(
                camPos, backdrop->voxelSizeM(), backdrop->chunkSizeVoxels());
            std::vector<ChunkCoord> resident;
            resident.reserve(backdropMeshes.size());
            for (const auto& kv : backdropMeshes) resident.push_back(kv.first);
            std::sort(resident.begin(), resident.end(),
                      [&](const ChunkCoord& a, const ChunkCoord& b) {
                          auto cheb = [&](const ChunkCoord& c) {
                              return std::max({std::abs(c.x - bc.x), std::abs(c.y - bc.y),
                                               std::abs(c.z - bc.z)});
                          };
                          return cheb(a) > cheb(b);
                      });
            for (const ChunkCoord& c : resident) {
                if (static_cast<int>(backdropMeshes.size()) <= backdropBudget) break;
                backdropMeshes[c].destroy();
                backdropMeshes.erase(c);
                backdrop->unloadChunk(c);
            }
        }

        // ── Trigger decomposition for nearby composite macro voxels ────────
        // Purely local: each side decomposes on its own approach from the shared
        // deterministic generators. Nothing crosses the wire here — the HUD
        // packet-rate counter stays flat while this runs.
        int enqueued = 0;
        for (const auto& kv : blocksMeshes) {
            if (enqueued >= kDecomposePerFrame) break;
            const Chunk* chunk = blocks->getChunk(kv.first);
            if (!chunk) continue;
            const int n = chunk->size();
            for (int z = 0; z < n && enqueued < kDecomposePerFrame; ++z)
                for (int y = 0; y < n && enqueued < kDecomposePerFrame; ++y)
                    for (int x = 0; x < n && enqueued < kDecomposePerFrame; ++x) {
                        if (chunk->at(x, y, z).isEmpty()) continue;
                        const chunkmath::VoxelCoord V =
                            chunkmath::chunkLocalToVoxel(kv.first, x, y, z, n);
                        if (!decomp.needsDecompose(V)) continue;
                        const WorldCoord ctr =
                            chunkmath::voxelCenter(V, blocks->voxelSizeM());
                        if (glm::length(ctr.value - camPos.value) > kDecomposeRadiusM) continue;
                        if (!decomp.markPending(V)) continue;
                        DecompositionJob job;
                        job.macro           = V;
                        job.childChunks     = childChunksForMacro(V, *blocks, *terrain);
                        job.childChunkSize  = terrain->chunkSizeVoxels();
                        job.childVoxelSizeM = terrain->voxelSizeM();
                        job.generator       = terrainGen.fn;
                        job.userData        = terrainGen.user_data;
                        worker.enqueue(job);
                        ++enqueued;
                    }
        }

        // ── Integrate completed decomposition jobs ─────────────────────────
        // Already-resident terrain chunks win over freshly generated ones: they
        // are player-edited state from the join handshake, the live edit stream,
        // or the host's save — exactly the data determinism cannot re-derive.
        std::unordered_set<ChunkCoord, ChunkCoordHash> compositeToRemesh;
        for (DecompositionResult& result : worker.drain()) {
            for (auto& chunkPtr : result.chunks) {
                const ChunkCoord tcc = chunkPtr->coord();
                Chunk* resident = const_cast<Chunk*>(terrain->getChunk(tcc));
                if (!resident && isHost && save.hasChunk(tcc)) {
                    if (auto disk = save.tryLoadChunk(tcc))
                        resident = terrain->insertChunk(std::move(disk));
                }
                if (!resident) resident = terrain->insertChunk(std::move(chunkPtr));
                if (!resident) continue;
                auto it = terrainMeshes.find(tcc);
                if (it != terrainMeshes.end()) {
                    it->second.destroy();
                    it->second = ChunkMesh::build(*resident);
                } else {
                    terrainMeshes.emplace(tcc, ChunkMesh::build(*resident));
                }
            }
            const chunkmath::LocalVoxel lv =
                chunkmath::voxelToChunkLocal(result.macro, blocks->chunkSizeVoxels());
            auto cit = blocks->chunks().find(lv.chunk);
            if (cit != blocks->chunks().end()) {
                if (!haveBlockTemplate) {
                    blockTemplate     = cit->second->at(lv.x, lv.y, lv.z);
                    haveBlockTemplate = true;
                }
                cit->second->at(lv.x, lv.y, lv.z) = Voxel::empty();
                compositeToRemesh.insert(lv.chunk);
            }
            decomp.markDecomposed(result.macro);
        }

        // ── Release decomposed terrain past the keep radius ────────────────
        std::vector<ChunkCoord> terrainToEvict;
        for (const auto& kv : terrainMeshes) {
            const chunkmath::VoxelCoord V{kv.first.x, kv.first.y, kv.first.z};
            const WorldCoord ctr = chunkmath::voxelCenter(V, blocks->voxelSizeM());
            if (glm::length(ctr.value - camPos.value) > kTerrainKeepRadiusM)
                terrainToEvict.push_back(kv.first);
        }
        for (const ChunkCoord& tcc : terrainToEvict) {
            if (isHost && terrain->isChunkDirty(tcc)) {
                if (const Chunk* ch = terrain->getChunk(tcc)) save.saveChunk(*ch);
                terrain->clearChunkDirty(tcc);
            }
            auto it = terrainMeshes.find(tcc);
            if (it != terrainMeshes.end()) {
                it->second.destroy();
                terrainMeshes.erase(it);
            }
            terrain->unloadChunk(tcc);
            const chunkmath::VoxelCoord V{tcc.x, tcc.y, tcc.z};
            decomp.clear(V);
            if (haveBlockTemplate) {
                const chunkmath::LocalVoxel lv =
                    chunkmath::voxelToChunkLocal(V, blocks->chunkSizeVoxels());
                auto cit = blocks->chunks().find(lv.chunk);
                if (cit != blocks->chunks().end()) {
                    cit->second->at(lv.x, lv.y, lv.z) = blockTemplate;
                    compositeToRemesh.insert(lv.chunk);
                }
            }
        }

        for (const ChunkCoord& c : compositeToRemesh) {
            const Chunk* chunk = blocks->getChunk(c);
            if (!chunk) continue;
            auto it = blocksMeshes.find(c);
            if (it != blocksMeshes.end()) {
                it->second.destroy();
                it->second = ChunkMesh::build(*chunk);
            }
        }

        // ── Targeting and edits — all through the applyEdit choke point ────
        glm::dvec3 lookDir{static_cast<double>(cp * sy), static_cast<double>(sp),
                           static_cast<double>(cp * cy)};
        voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);

        bool curLeft  = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS);
        bool curRight = (glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
        if (!g_chatInputActive && hit.hit) {
            if (curLeft && !prevLeft) {
                nm.applyEdit(net::kLocalPlayer,
                             chunkmath::voxelCenter(hit.voxel, terrain->voxelSizeM()),
                             Voxel::empty());
            }
            if (curRight && !prevRight) {
                Voxel placed;
                placed.material = pluginManager.material(buildMaterials[selectedMaterial]);
                nm.applyEdit(net::kLocalPlayer,
                             chunkmath::voxelCenter(hit.adjacent, terrain->voxelSizeM()),
                             placed);
            }
        }
        prevLeft  = curLeft;
        prevRight = curRight;

        if (hit.hit) {
            renderer.drawVoxelHighlight(
                chunkmath::voxelCenter(hit.voxel, terrain->voxelSizeM()),
                static_cast<float>(terrain->voxelSizeM()), 0xff00ffff);
        }

        // ── Host autosave ──────────────────────────────────────────────────
        if (isHost) {
            saveTimer += dt;
            if (saveTimer >= kSaveIntervalS) {
                saveTimer = 0.0;
                save.saveDirtyChunks(world);
            }
        }

        // ── HUD ────────────────────────────────────────────────────────────
        packetTimer += dt;
        if (packetTimer >= 1.0) {
            packetRate      = static_cast<double>(nm.packetsReceived() - lastPacketCount) /
                              packetTimer;
            lastPacketCount = nm.packetsReceived();
            packetTimer     = 0.0;
        }

        std::vector<std::string> hud;
        {
            char line[128];
            std::string rtt;
            for (const auto& kv : nm.playerPositions()) {
                char r[32];
                std::snprintf(r, sizeof(r), " p%u:%ums", kv.first,
                              nm.rttMs(isHost ? kv.first : 1));
                rtt += r;
            }
            std::snprintf(line, sizeof(line), "%s | players: %zu | rtt%s",
                          isHost ? "HOST (authority)"
                                 : (nm.joinComplete() ? "CLIENT" : "CLIENT (joining...)"),
                          nm.connectedPeerCount() + 1, rtt.empty() ? " -" : rtt.c_str());
            hud.emplace_back(line);
            const char* mode =
                interestUi == InterestUi::BroadcastAll    ? "broadcast-all" :
                interestUi == InterestUi::StreamingRadius ? "streaming-radius"
                                                          : "plugin-filter(48m)";
            std::snprintf(line, sizeof(line),
                          "pkts/s: %.0f | seed: %llu | interest: %s | suppressed: %llu",
                          packetRate,
                          static_cast<unsigned long long>(nm.worldSeed()),
                          isHost ? mode : "(host-side)",
                          static_cast<unsigned long long>(nm.suppressedEditCount()));
            hud.emplace_back(line);
            std::snprintf(line, sizeof(line), "last edit: %s | material: %s",
                          lastEditSource.c_str(),
                          buildMaterials[selectedMaterial].c_str());
            hud.emplace_back(line);
        }
        for (const std::string& chat : g_chatLog) hud.push_back(chat);
        if (g_chatInputActive) hud.push_back("say: " + g_chatInput + "_");
        renderer.setHudText(hud);

        // Resize.
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        // ── Render: every layer at its own scale + remote player markers ───
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        for (const auto& kv : backdropMeshes) {
            const Chunk* chunk = backdrop->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), backdrop->voxelSizeM(), backdrop->chunkSizeVoxels());
        }
        for (const auto& kv : blocksMeshes) {
            const Chunk* chunk = blocks->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), blocks->voxelSizeM(), blocks->chunkSizeVoxels());
        }
        for (const auto& kv : terrainMeshes) {
            const Chunk* chunk = terrain->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), terrain->voxelSizeM(), terrain->chunkSizeVoxels());
        }
        // Remote players: a colored marker cube at each last-known position.
        for (const auto& kv : nm.playerPositions()) {
            renderer.drawVoxel(kv.second,
                               kPlayerColors[kv.first %
                                             (sizeof(kPlayerColors) / sizeof(kPlayerColors[0]))]);
        }
        renderer.render();
    }

    if (isHost) {
        int saved = save.saveDirtyChunks(world);
        Log::info(kLogCat, (std::string("Saved ") + std::to_string(saved)
                            + " edited chunk(s) to " + save.directory()).c_str());
    }
    nm.stop();  // graceful disconnect — fires on_player_left on the remote side
    g_net = nullptr;

    for (auto& kv : blocksMeshes)   kv.second.destroy();
    for (auto& kv : backdropMeshes) kv.second.destroy();
    for (auto& kv : terrainMeshes)  kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}

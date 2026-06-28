# Tutorial 11: Multiplayer

Add multiplayer to your game with edit replication, custom messaging, player
lifecycle hooks, and visible player markers using the host-as-authority
networking model.

---

## Prerequisites

- The engine builds and runs successfully (see
  [Tutorial 01](01-hello-voxel.md))
- Familiarity with the plugin system ([Tutorial 02](02-your-first-plugin.md))
- Understanding of world edits via `apply_edit`
  ([Tutorial 08](08-camera-raycasting-interaction.md))
- Knowledge of the `on_voxel_modified` callback
  ([Tutorial 09](09-player-mechanics.md))

---

## 1. The host-as-authority P2P model

VoxelEngine uses a host-as-authority peer-to-peer model. One instance acts
as both the authoritative server and a local client (the "host peer").
Other instances connect as pure clients. There is no dedicated server
binary -- the host is a regular game instance that also validates edits.

### Setting up the NetworkManager

```cpp
net::NetworkManager nm;
nm.init(world, pluginManager);
```

### Starting as host

```cpp
nm.setWorldSave(&save);
nm.setWorldSeed(kWorldSeed);
nm.setLayerConfig(&layerConfig);
nm.startHostPeer(27777);  // port; runs authority + local client
```

The host owns the canonical world state. It validates all incoming edits,
commits them, and broadcasts the results to connected peers.

### Starting as client

```cpp
nm.startClient("localhost", 27777);
```

The client receives the world seed and layer config from the host, then
streams chunk data as needed. Call `nm.joinComplete()` to check when the
initial world state has synced:

```cpp
if (nm.joinComplete()) {
    // World is loaded, safe to start gameplay
}
```

### Wiring into the engine

```cpp
engine.setNetworkManager(&nm);
```

Per-tick update:

```cpp
nm.update(dt);
```

---

## 2. Edit replication

All world edits flow through a single choke point -- `applyEdit` on the
NetworkManager (or `apply_edit` via the plugin context). This ensures every
edit is validated, committed, and replicated consistently.

```cpp
// From game code:
nm.applyEdit(kLocalPlayer, position, newVoxel);

// From plugin code:
ctx->apply_edit(ctx, position, &voxel);
```

The replication flow:

1. **Client** calls `applyEdit` -- the edit is forwarded to the host.
2. **Host** validates the edit (via authority policy, see section 3).
3. **Host** commits the edit to the world.
4. **Host** broadcasts the committed edit to all peers (filtered by interest,
   see section 5).
5. **Each peer** applies the edit locally and fires `on_voxel_modified`.

This happens within one round-trip. The client does not apply the edit
locally until it receives confirmation from the host (authoritative model).

---

## 3. Custom authority policy

The host can validate edit intents before they are committed. Register an
authority policy to implement access control, protected regions, or
game-rule enforcement:

```cpp
ctx->register_authority_policy(ctx, [](PlayerId peer, WorldCoord pos,
    const Voxel* proposed, void* ud) -> bool {
    // Return true to allow the edit, false to reject it

    // Example: protect a spawn region
    if (glm::length(pos.value - glm::dvec3(0, 0, 0)) < 10.0)
        return false;  // reject edits near spawn

    return true;
}, nullptr);
```

The policy runs on the host only. If it returns `false`, the edit is silently
dropped -- the requesting client receives no confirmation and the world
remains unchanged.

---

## 4. Edit conflict resolution

For more nuanced control than accept/reject, register an edit-received
handler that can transform edits:

```cpp
ctx->register_on_edit_received(ctx, [](PlayerId proposer, WorldCoord pos,
    const Voxel* proposed, Voxel* out_voxel, void* ud) -> EditResolution {

    // EditResolution::Apply    -- accept the edit as-is
    // EditResolution::Discard  -- reject the edit
    // EditResolution::Transform -- commit *out_voxel instead of *proposed

    // Example: downgrade a high-density material for non-admin players
    if (proposed->material.density > 5000.0f && proposer != kAdminPlayer) {
        *out_voxel = *proposed;
        out_voxel->material.density = 2600.0f;
        return EditResolution::Transform;
    }

    return EditResolution::Apply;
}, nullptr);
```

The three resolution modes:

| Resolution | Effect |
|------------|--------|
| `Apply` | Commit `proposed` as-is |
| `Discard` | Drop the edit entirely |
| `Transform` | Commit `*out_voxel` (your modified version) instead |

---

## 5. Interest filtering

Not every peer needs every edit. Interest filtering controls which edits
reach which clients, saving bandwidth in large worlds.

### Built-in modes

```cpp
// Every edit goes to every peer (simple, high bandwidth):
nm.setInterestMode(net::InterestMode::BroadcastAll);

// Only send edits within streaming radius of each peer:
nm.setInterestMode(net::InterestMode::StreamingRadius);
```

### Custom filter

For game-specific rules (team visibility, fog of war, dimension separation):

```cpp
ctx->register_interest_filter(ctx, [](PlayerId target, WorldCoord editPos,
    void* ud) -> bool {
    // Return true if 'target' should receive the edit at 'editPos'

    // Example: only send edits within 200m of the target player
    WorldCoord targetPos = getPlayerPosition(target);
    return glm::length(editPos.value - targetPos.value) < 200.0;
}, nullptr);
```

The trade-off: `BroadcastAll` is simplest but scales poorly.
`StreamingRadius` is a good default for open-world games. Custom filters
give full control but require careful design to avoid desyncs (a player
who walks into a region they were filtered out of needs a chunk re-sync).

---

## 6. Custom network messages

Beyond edit replication, you can send arbitrary messages between peers using
channel-based messaging.

### MessageEnvelope

```cpp
struct MessageEnvelope {
    const char* channel_id;           // e.g. "engine.chat"
    PlayerId sender_id = 0;           // filled by the engine on receive
    PlayerId target_player = 0;       // for targeted messages
    MessageTarget target = MessageTarget::Broadcast;
    MessageReliability reliability = MessageReliability::Reliable;
    const void* payload;
    size_t payload_size;
};
```

### Sending a message

```cpp
std::string text = "Hello, world!";

MessageEnvelope env{};
env.channel_id  = "engine.chat";
env.target      = MessageTarget::Broadcast;
env.reliability = MessageReliability::Reliable;
env.payload      = text.c_str();
env.payload_size = text.size();

ctx->send_network_message(ctx, &env);
```

For targeted messages, set `env.target = MessageTarget::Player` and fill in
`env.target_player`.

### Receiving messages

Subscribe to a channel prefix to receive messages:

```cpp
ctx->register_on_network_message(ctx, "engine.chat",
    [](const MessageEnvelope* env, void* ud) {
        std::string msg(static_cast<const char*>(env->payload),
                        env->payload_size);
        // env->sender_id identifies who sent it
        printf("[Player %u]: %s\n", env->sender_id, msg.c_str());
    }, nullptr);
```

The prefix match means registering for `"engine."` would receive both
`"engine.chat"` and `"engine.status"` messages. Use this for plugin
namespacing.

### The chat plugin as a worked example

The `chat` plugin (`plugins/chat/plugin.cpp`) demonstrates the full message
pattern:

1. Registers a handler for `"engine.chat"` messages.
2. Exposes a function to send chat text.
3. On receive, formats and displays the message with the sender's player ID.

Study it as a minimal reference for any custom messaging feature (trade
requests, team commands, game events).

---

## 7. Player lifecycle hooks

React to players joining and leaving the session:

```cpp
ctx->register_on_player_joined(ctx, [](PlayerId id, WorldCoord pos, void* ud) {
    printf("Player %u joined at (%.0f, %.0f, %.0f)\n",
           id, pos.value.x, pos.value.y, pos.value.z);
    // Spawn their avatar, send welcome message, etc.
}, nullptr);

ctx->register_on_player_left(ctx, [](PlayerId id, void* ud) {
    printf("Player %u left\n", id);
    // Remove avatar, clean up resources
}, nullptr);
```

These fire on the host for all peers, and on each client for remote peers.

---

## 8. Player voxel representation

Remote players need a visible presence in the world. The engine does not
impose an avatar model -- instead, use the immediate-mode `drawVoxel` API
to render colored markers at each peer's position:

```cpp
const uint32_t kPlayerColors[] = {0xff0000ff, 0xff00ff00, 0xffff8800, 0xffff00ff};

for (const auto& [playerId, pos] : nm.playerPositions()) {
    renderer.drawVoxel(pos, kPlayerColors[playerId % 4]);
}
```

### Key properties of drawVoxel markers

- **Per-frame / immediate-mode**: the marker is submitted each frame and is
  transient. It is never placed in the world grid.
- **No cleanup needed**: when a peer disconnects, you simply stop drawing
  their marker. There is nothing to remove from the world.
- **No collision or physics**: `drawVoxel` is rendering-only. The marker
  does not participate in collision detection or persistence.

### Multi-voxel figures

For a more readable player representation, use multiple `drawVoxel` calls
per peer to build a small figure:

```cpp
for (const auto& [playerId, pos] : nm.playerPositions()) {
    uint32_t color = kPlayerColors[playerId % 4];
    // Body (1x1x1 at feet level):
    renderer.drawVoxel(pos, color);
    // Head (1x1x1 above body):
    WorldCoord headPos(pos.value + glm::dvec3(0, 1, 0));
    renderer.drawVoxel(headPos, color);
}
```

### Position broadcasting

Each frame, broadcast the local player's position so others can see it:

```cpp
nm.broadcastLocalPosition(camPos);
```

On the host, peer positions are tracked automatically:

```cpp
nm.updatePeerPosition(playerId, pos);
```

---

## 9. NetworkManager queries

The `NetworkManager` exposes several status queries for UI and diagnostics:

| Method | Return | Description |
|--------|--------|-------------|
| `nm.role()` | `SessionRole` | `Offline`, `Server`, `Client`, or `HostPeer` |
| `nm.isActive()` | `bool` | True if networking is running |
| `nm.localPlayerId()` | `PlayerId` | This instance's player ID |
| `nm.connectedPeerCount()` | `uint32_t` | Number of connected peers |
| `nm.packetsReceived()` | `uint64_t` | Total packets received (diagnostics) |
| `nm.rttMs(PlayerId)` | `float` | Smoothed round-trip time to a peer |
| `nm.joinComplete()` | `bool` | Client: true when world state is synced |

Use `rttMs` to display latency in the HUD (see
[Tutorial 09](09-player-mechanics.md) for HUD integration). Use
`connectedPeerCount` for player-count displays.

---

## 10. Putting it all together

A condensed host setup showing all the pieces:

```cpp
net::NetworkManager nm;
nm.init(world, pluginManager);
engine.setNetworkManager(&nm);

// Authority policy: protect spawn
ctx->register_authority_policy(ctx, [](PlayerId peer, WorldCoord pos,
    const Voxel* proposed, void* ud) -> bool {
    return glm::length(pos.value) >= 10.0;  // reject edits near origin
}, nullptr);

// Player lifecycle
ctx->register_on_player_joined(ctx, [](PlayerId id, WorldCoord pos, void* ud) {
    printf("Player %u joined\n", id);
}, nullptr);

// Chat messaging
ctx->register_on_network_message(ctx, "engine.chat",
    [](const MessageEnvelope* env, void* ud) {
        std::string msg(static_cast<const char*>(env->payload), env->payload_size);
        printf("[%u]: %s\n", env->sender_id, msg.c_str());
    }, nullptr);

// Start hosting
nm.startHostPeer(27777);

while (!window.shouldClose()) {
    nm.update(dt);
    engine.update(dt);

    // Broadcast our position
    nm.broadcastLocalPosition(camPos);

    // Draw remote players
    for (const auto& [pid, pos] : nm.playerPositions())
        renderer.drawVoxel(pos, kPlayerColors[pid % 4]);

    // World edits go through the network manager:
    if (placingVoxel)
        nm.applyEdit(nm.localPlayerId(), targetPos, newVoxel);

    renderer.renderWorld(world);
    renderer.render();
}
```

---

## Challenge: add a protected zone and a join message

Combine an authority policy, a custom message channel, and a lifecycle hook.

1. Register an `authority_policy` on the host that rejects edits inside a small
   box around spawn (section 3).
2. Register a handler on a new channel (e.g. `"game.ping"`) and broadcast a
   short message from the `on_player_joined` hook when a player connects.
3. Run a host and a client. From the client, try to edit inside the protected
   zone -- it should be silently refused on both windows -- and confirm the join
   message prints.

<details>
<summary>Stuck? Where to look</summary>

- Demo: `demos/11-shared-world/main.cpp`; reference plugins
  `plugins/chat/plugin.cpp` and `plugins/server-authority/plugin.cpp`.
- `register_authority_policy` (section 3), custom messaging (section 6), and
  `register_on_player_joined` (section 7) are the hooks you need.
- Interest modes are set with `nm.setInterestMode(...)` (section 5).

</details>

**Going further:** switch `setInterestMode` to `StreamingRadius` and verify
edits far from a peer no longer reach it.

---

## How to verify

Build and run two instances of the shared-world demo:

```bash
cmake -B build && cmake --build build

# Terminal 1 -- host:
./build/11-shared-world --host

# Terminal 2 -- client (same machine):
./build/11-shared-world --join localhost
```

You should see:

- Both windows show the same world.
- Edits made in one window appear in the other within one round-trip.
- Each player sees the other as a colored voxel marker that moves in
  real-time.
- The host's authority policy is enforced -- edits rejected by the policy
  do not appear on any client.
- If you close the client window, the host continues running and the
  player marker disappears.

---

## Key references

| What | Where |
|------|-------|
| NetworkManager API | `src/net/NetworkManager.h` |
| Networking plugin hooks | `include/plugin_api.h` (authority, interest, messaging) |
| Chat plugin (messaging example) | `plugins/chat/plugin.cpp` |
| Server authority plugin | `plugins/server-authority/plugin.cpp` |
| Demo source | `demos/11-shared-world/main.cpp` |
| Player marker rendering | `demos/11-shared-world/main.cpp` (lines 914-919) |
| Edit system | [Tutorial 08](08-camera-raycasting-interaction.md) |
| HUD integration | [Tutorial 09](09-player-mechanics.md) |
| Architecture overview | [`docs/architecture.md`](../architecture.md) |

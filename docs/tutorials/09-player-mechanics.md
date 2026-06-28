# Tutorial 09: Player Mechanics

Wire up first-person walk/mine/build gameplay with a full game HUD, using the
kinematic-body plugin for physics, input plugins for device-agnostic controls,
and the cell-grid overlay API for heads-up display elements.

---

## Prerequisites

- The engine builds and runs successfully (see
  [Tutorial 01](01-hello-voxel.md))
- Familiarity with the plugin system ([Tutorial 02](02-your-first-plugin.md))
- Understanding of raycasting and world edits
  ([Tutorial 08](08-camera-raycasting-interaction.md))
- A C++17 compiler

---

## 1. The kinematic-body plugin

The `kinematic-body` plugin provides a body registry with gravity, jumping,
and sweep-and-resolve AABB collision against the voxel world. It is a
standalone plugin under `plugins/kinematic-body/` -- load it like any other
plugin.

### Creating a body

Every moving entity in your game is a **body**, identified by a `BodyId`
(a `uint32_t`, with `kInvalidBody = 0` as the null sentinel). You create
one by filling out a `BodyDesc`:

```cpp
#include "kinematic_body.h"

kinbody::BodyDesc desc;
desc.center = WorldCoord(0.5, 27.0, 0.5);
desc.half_x = 0.3;           // AABB half-extents
desc.half_y = 0.9;
desc.half_z = 0.3;
desc.eye_offset = 0.7;       // camera height above center
desc.walk_speed = 8.0;       // m/s
desc.gravity_accel = 25.0;   // m/s^2
desc.jump_speed = 9.0;       // m/s
desc.gravity_dir_x = 0.0;
desc.gravity_dir_y = -1.0;
desc.gravity_dir_z = 0.0;

kinbody::BodyId player = kinbody::api().create_body(&desc);
```

The `BodyDesc` struct captures everything about a body's shape and movement
tuning:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `center` | `WorldCoord` | -- | Initial spawn position |
| `half_x`, `half_y`, `half_z` | `double` | 0.3, 0.9, 0.3 | AABB half-extents |
| `eye_offset` | `double` | 0.7 | Camera height above center |
| `walk_speed` | `double` | 8.0 | Movement speed in m/s |
| `gravity_accel` | `double` | 25.0 | Gravity acceleration in m/s^2 |
| `jump_speed` | `double` | 9.0 | Initial upward velocity on jump |
| `gravity_dir_*` | `double` | (0, -1, 0) | Gravity direction vector |

### Per-frame input

Each frame you tell the body what the player wants to do via `BodyInput`:

```cpp
kinbody::BodyInput in{};
in.wish_x = forward * sin(yaw) + strafe * cos(yaw);
in.wish_z = forward * cos(yaw) - strafe * sin(yaw);
in.jump = jumpPressed;

kinbody::api().set_input(player, &in);
```

The `wish_*` fields are a world-space wish direction (not velocity). The
plugin clamps and scales them by `walk_speed` internally.

### Reading state after the step

After the engine ticks, read back the resolved state:

```cpp
const kinbody::BodyState* st = kinbody::api().get_state(player);

WorldCoord eyePos(st->center.value + glm::dvec3(0, desc.eye_offset, 0));
bool onGround = st->grounded;
```

The `BodyState` struct contains:

| Field | Type | Description |
|-------|------|-------------|
| `center` | `WorldCoord` | Post-collision position |
| `vel_x`, `vel_y`, `vel_z` | `double` | Current velocity |
| `grounded` | `bool` | True if resting on a surface |
| `hit_x`, `hit_y`, `hit_z` | `bool` | Axis-aligned collision flags |

### Other body operations

```cpp
// Teleport / respawn:
kinbody::api().set_position(player, WorldCoord(0.5, 50.0, 0.5));

// Change gravity per body (e.g. low-gravity zone):
kinbody::api().set_gravity(player, 0.0, -1.0, 0.0, 10.0);

// Destroy when the player disconnects:
kinbody::api().destroy_body(player);

// Query how many bodies exist:
uint32_t count = kinbody::api().body_count();
```

---

## 2. The register_on_tick hook

The kinematic-body plugin steps all bodies during the engine tick. Internally
it registers a tick callback:

```cpp
ctx->register_on_tick(ctx, tick, nullptr);
```

Inside its tick function the plugin calls `ctx->move_aabb` for each body:

```cpp
BodyMoveResult result = ctx->move_aabb(ctx,
    center, half_x, half_y, half_z,
    dx, dy, dz,
    grav_x, grav_y, grav_z);
```

The `BodyMoveResult` contains `position`, `grounded`, `hitX`, `hitY`, and
`hitZ` -- the sweep-and-resolve collision output. You do not need to call
`move_aabb` directly; the kinematic-body plugin handles it. But understanding
the flow matters when debugging collision edge cases.

If you need your own per-frame logic (AI, animation), register an additional
tick callback:

```cpp
ctx->register_on_tick(ctx, my_tick_fn, my_user_data);
```

---

## 3. Input plugins

The engine ships two input plugins that wrap GLFW into a device-agnostic
query API.

### Keyboard and mouse

```cpp
#include "keyboard-mouse/plugin.h"  // or via plugin_api.h

kbinput::api().set_source(&kbSrc);

// Bind axes (negative key, positive key):
kbinput::api().bind_axis("forward", GLFW_KEY_S, GLFW_KEY_W);
kbinput::api().bind_axis("strafe",  GLFW_KEY_A, GLFW_KEY_D);

// Bind discrete keys:
kbinput::api().bind_key("jump", GLFW_KEY_SPACE);

// Bind mouse buttons:
kbinput::api().bind_mouse_button("mine",  GLFW_MOUSE_BUTTON_LEFT);
kbinput::api().bind_mouse_button("place", GLFW_MOUSE_BUTTON_RIGHT);

// Sensitivity:
kbinput::api().set_mouse_sensitivity(0.0022);
```

Query each frame:

```cpp
float fwd   = kbinput::api().axis("forward");   // -1.0 to 1.0
bool  mining = kbinput::api().held("mine");      // held this frame
bool  jumped = kbinput::api().pressed("jump");   // pressed this frame (edge)

double dx, dy;
kbinput::api().mouse_delta(&dx, &dy);            // raw mouse delta
```

### Gamepad

```cpp
gpinput::api().set_source(&gpSrc);

gpinput::api().bind_button("jump", GLFW_GAMEPAD_BUTTON_A);
gpinput::api().bind_trigger("mine", gpinput::AxisRightTrigger);

float lx, ly;
gpinput::api().stick(gpinput::StickLeft, &lx, &ly);  // left stick axes
```

### Auto device switching

Track which device last produced input and set `activeDevice` accordingly.
A simple pattern:

```cpp
enum class Device { Keyboard, Gamepad };
Device activeDevice = Device::Keyboard;

// Each frame:
if (kbinput::api().any_activity())
    activeDevice = Device::Keyboard;
if (gpinput::api().any_activity())
    activeDevice = Device::Gamepad;
```

Display the active device on the HUD status line (section 7) so the player
knows which control scheme is live.

---

## 4. Hardness-gated mining with visual feedback

Mining is not instant -- every material has a `hardness` value (see
[Tutorial 03](03-materials-and-properties.md)), and the player accumulates
removal progress over time.

```cpp
sim::RemovalAccumulator remover;

// Each frame, when aiming at a voxel and holding the mine button:
if (hit.hit && mining) {
    if (remover.accrue(hit.voxel, target.material.hardness, kToolPower, dt)) {
        // Voxel fully mined -- break it:
        Voxel empty = Voxel::empty();
        ctx->apply_edit(ctx, hit.voxel, &empty);
        remover.reset();
    }
}

// Visual feedback: the progress ramp (0.0 to 1.0):
float progress = remover.progress();
renderer.drawVoxelHighlight(hit.voxel, voxelSize, highlightColor, progress);
```

When the player looks away from the target voxel, call `remover.reset()` to
restart the accumulation. The `drawVoxelHighlight` progress parameter drives
the visual crack/ramp effect on the targeted block (see
[Tutorial 08](08-camera-raycasting-interaction.md) for the highlight API).

---

## 5. The on_voxel_modified callback

React to any block change in the world -- whether from mining, placing, or
network replication:

```cpp
ctx->register_on_voxel_modified(ctx, [](WorldCoord pos, const Voxel* old_v,
    const Voxel* new_v, PlayerId source, void* ud) {
    // old_v: what was there before
    // new_v: what is there now
    // source: which player made the change

    if (new_v->isEmpty()) {
        // A voxel was removed -- update inventory, play break sound
    } else if (old_v->isEmpty()) {
        // A voxel was placed
    }
}, userData);
```

This is the same callback the `material-audio` plugin uses to trigger
break/place sounds (see [Tutorial 10](10-audio-and-sound.md)). It fires for
all edit sources -- local player, remote peers, and plugin-driven edits.

---

## 6. HUD integration via cell-grid overlay API

The renderer exposes a text-mode overlay grid for HUD elements. Each cell
is a glyph + attribute byte, similar to a classic text-mode framebuffer.

### Setup

Call `hudClear()` once per frame before drawing any HUD elements:

```cpp
renderer.hudClear();
```

Query grid dimensions for layout calculations:

```cpp
int cols = renderer.hudCols();
int rows = renderer.hudRows();
```

### Text

```cpp
renderer.hudText(col, row, hud::attr(hud::White), "HP");
```

### Filled rectangles (health bar)

```cpp
int barWidth = 20;
int filled = static_cast<int>(hp * barWidth);

// Green filled portion:
renderer.hudFill(4, 1, filled, 1, hud::attr(hud::Black, hud::Green));
// Gray empty portion:
renderer.hudFill(4 + filled, 1, barWidth - filled, 1,
                 hud::attr(hud::Black, hud::DarkGray));
```

### Blit pre-built cell buffers (minimap)

For complex HUD elements like a minimap, build a cell buffer offline and blit
it in one call:

```cpp
renderer.hudCells(col, row, width, height, minimapData.data());
```

The data format is 2 bytes per cell: glyph + attribute.

### Color system

The `hud::Color` enum provides 16 colors:

> Black, Blue, Green, Cyan, Red, Magenta, Brown, LightGray, DarkGray,
> LightBlue, LightGreen, LightCyan, LightRed, LightMagenta, Yellow, White

Pack foreground and background into a single attribute byte:

```cpp
uint8_t attr = hud::attr(foreground, background);
// Internally: (bg << 4) | (fg & 0x0f)
```

### Inventory hotbar

Render material slots along the bottom of the screen:

```cpp
for (int i = 0; i < 5; ++i) {
    int col = cols / 2 - 5 + i * 2;
    int row = rows - 2;
    uint8_t a = (i == selectedSlot)
        ? hud::attr(hud::White, hud::LightBlue)
        : hud::attr(hud::LightGray, hud::Black);
    char label = '1' + i;
    renderer.hudText(col, row, a, std::string(1, label));
}
```

### Status line

Use the bottom row for coordinates, active device, and FPS:

```cpp
char statusBuf[128];
snprintf(statusBuf, sizeof(statusBuf),
         "X:%.0f Y:%.0f Z:%.0f | %s | %.0f FPS",
         st->center.value.x, st->center.value.y, st->center.value.z,
         activeDevice == Device::Keyboard ? "KB/Mouse" : "Gamepad",
         1.0 / dt);
renderer.hudText(0, rows - 1, hud::attr(hud::Yellow), statusBuf);
```

---

## 7. Putting it all together

A condensed game loop showing all the pieces wired together:

```cpp
kinbody::BodyDesc desc{};
desc.center = WorldCoord(0.5, 27.0, 0.5);
desc.eye_offset = 0.7;
kinbody::BodyId player = kinbody::api().create_body(&desc);

sim::RemovalAccumulator remover;

while (!window.shouldClose()) {
    double dt = timer.delta();
    window.pollEvents();

    // 1. Gather input
    float fwd    = kbinput::api().axis("forward");
    float strafe = kbinput::api().axis("strafe");
    bool  jump   = kbinput::api().pressed("jump");
    bool  mining = kbinput::api().held("mine");

    double mdx, mdy;
    kbinput::api().mouse_delta(&mdx, &mdy);
    yaw   += mdx;
    pitch  = glm::clamp(pitch - mdy, -1.55, 1.55);

    // 2. Feed kinematic body
    kinbody::BodyInput in{};
    in.wish_x = fwd * sin(yaw) + strafe * cos(yaw);
    in.wish_z = fwd * cos(yaw) - strafe * sin(yaw);
    in.jump   = jump;
    kinbody::api().set_input(player, &in);

    // 3. Step the engine (ticks all bodies)
    engine.update(dt);

    // 4. Read back state
    const kinbody::BodyState* st = kinbody::api().get_state(player);
    WorldCoord eye(st->center.value + glm::dvec3(0, desc.eye_offset, 0));

    // 5. Raycast for interaction
    glm::dvec3 dir(cos(pitch)*sin(yaw), sin(pitch), cos(pitch)*cos(yaw));
    auto hit = voxelcast::raycast(world, eye, dir, 8.0);

    // 6. Mining
    if (hit.hit && mining) {
        Voxel target = world.getVoxel(hit.voxel);
        if (remover.accrue(hit.voxel, target.material.hardness, kToolPower, dt)) {
            Voxel empty = Voxel::empty();
            ctx->apply_edit(ctx, hit.voxel, &empty);
            remover.reset();
        }
        renderer.drawVoxelHighlight(hit.voxel, 1.0, 0xff00ffff, remover.progress());
    } else {
        remover.reset();
    }

    // 7. HUD
    renderer.hudClear();
    // ... health bar, hotbar, status line (see section 6)

    // 8. Render
    renderer.setCameraPosition(eye);
    renderer.setCameraRotation(pitch, yaw, 0.0f);
    renderer.renderWorld(world);
    renderer.render();
}
```

---

## Challenge: add a low-gravity toggle with HUD feedback

Tie together the kinematic body, input bindings, and the cell-grid HUD.

1. Bind an unused key with `kbinput::api().bind_key(...)` that calls
   `set_gravity` on the player body to toggle a low-gravity mode (e.g.
   `gravity_accel` 25 -> 6).
2. Add a HUD status item via `hudText` that shows the current gravity mode.
3. Rebuild and run `18-hud-and-controls`. Toggle the mode mid-jump and feel the
   difference; confirm the HUD label updates.

<details>
<summary>Stuck? Where to look</summary>

- Demo: `demos/18-hud-and-controls/main.cpp`.
- Toggle gravity with `kinbody::api().set_gravity(...)` (section 1); bind the
  key via `kbinput::api().bind_key(...)` (section 3).
- Draw the label with `renderer.hudText(...)` (section 6); airborne state is
  `BodyState::grounded` (section 1).

</details>

**Going further:** read `BodyState::grounded` each frame and tint the crosshair
differently when airborne versus standing on a surface.

---

## How to verify

Build and run the HUD-and-controls demo:

```bash
cmake -B build && cmake --build build
./build/18-hud-and-controls
```

You should see:

- First-person movement with WASD, mouse look, and space to jump.
- Gravity pulls the player down; jumping launches upward.
- Left-click mines voxels with a progress indicator; harder materials take
  longer.
- A health bar, inventory hotbar, coordinate readout, and FPS counter are
  drawn on screen via the cell-grid HUD.
- If a gamepad is connected, grabbing it switches the active device
  indicator automatically.

---

## Key references

| What | Where |
|------|-------|
| Kinematic body API | `plugins/kinematic-body/kinematic_body.h` |
| Kinematic body implementation | `plugins/kinematic-body/plugin.cpp` |
| Keyboard-mouse input plugin | `plugins/keyboard-mouse/plugin.cpp` |
| Gamepad input plugin | `plugins/gamepad/plugin.cpp` |
| HUD overlay API | `src/renderer/BgfxRenderer.h` |
| Removal accumulator | `include/plugin_api.h` (`RemovalAccumulator`) |
| Voxel highlight | `include/renderer/Renderer.h` (`drawVoxelHighlight`) |
| Demo source | `demos/18-hud-and-controls/main.cpp` |
| Material properties (hardness) | [Tutorial 03](03-materials-and-properties.md) |
| Raycasting and edits | [Tutorial 08](08-camera-raycasting-interaction.md) |
| Audio feedback on edits | [Tutorial 10](10-audio-and-sound.md) |
| Architecture overview | [`docs/architecture.md`](../architecture.md) |

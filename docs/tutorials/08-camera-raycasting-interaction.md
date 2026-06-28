# Tutorial 08: Camera, Raycasting, and Interaction

Aim at voxels, break them, place new ones, and render visual feedback including
a mining-laser beam. This tutorial covers the three primitives every interactive
voxel game needs: camera control, picking, and world edits.

---

## Prerequisites

- Engine built and running ([Tutorial 01](01-hello-voxel.md))
- Comfortable writing a plugin ([Tutorial 02](02-your-first-plugin.md))
- Familiar with materials and palette colors ([Tutorial 03](03-materials-and-properties.md))
- Understanding of the World API and WorldCoord ([Tutorial 01](01-hello-voxel.md), section 5)

---

## 1. Camera modes

### Fly-camera

Set the camera position and rotation each frame via the renderer:

```cpp
renderer.setCameraPosition(camPos);           // WorldCoord (double precision)
renderer.setCameraRotation(pitch, yaw, 0.0f); // radians
```

`camPos` is a `WorldCoord` wrapping `glm::dvec3`. The GPU only receives 32-bit
floats -- before submission, all geometry is translated into camera-local space
so the floats that reach the shader are always small and precise.

### Walk-mode (G toggle)

Many demos toggle between fly-camera and walk-mode with the **G** key.
Walk-mode uses the AABB collision primitive to resolve the player against solid
voxels:

```cpp
#include "world/VoxelCollision.h"

voxelcollide::MoveResult mr = voxelcollide::moveAABB(
    world,
    {playerCenter, kPlayerHalf},   // AABB: center + half-extents
    delta                          // displacement this frame
);
playerCenter = mr.position;
grounded = mr.grounded;
```

`MoveResult` reports the resolved position and whether the player is resting on
a surface:

```cpp
struct MoveResult {
    WorldCoord position;          // resolved center after the move
    bool       grounded = false;  // resting on a solid surface (blocked along gravity)
    bool       hitX = false, hitY = false, hitZ = false;
};
```

The collision is substepped (no step larger than half a voxel) so the player
cannot tunnel through thin geometry at speed. The `grounded` flag is true when
the move was blocked along the gravity vector -- use it to gate jumping.

---

## 2. Building a ray from the camera

To pick voxels, cast a ray from the camera position along the look direction.
Compute the look direction from pitch and yaw:

```cpp
float cp = std::cos(pitch), sp = std::sin(pitch);
float cy = std::cos(yaw),   sy = std::sin(yaw);
glm::dvec3 lookDir{cp * sy, sp, cp * cy};
```

The ray origin is `camPos` (the camera's `WorldCoord`).

---

## 3. Raycasting / voxel picking

The `voxelcast::raycast` API performs DDA grid traversal in double precision
against the world's resident terminal voxels:

```cpp
#include "world/VoxelRaycast.h"

voxelcast::RayHit hit = voxelcast::raycast(world, camPos, lookDir, kReachM);
```

Where `kReachM` is the maximum ray distance in meters (e.g. `8.0` for
standard reach).

### The RayHit struct

```cpp
struct RayHit {
    bool                  hit;       // false if nothing solid within reach
    chunkmath::VoxelCoord voxel;     // solid voxel struck (removal target)
    chunkmath::VoxelCoord adjacent;  // empty cell on entry face (placement target)
    glm::ivec3            normal;    // face normal pointing back toward ray origin
    double                distance;  // world-space distance to hit
};
```

| Field | Usage |
|-------|-------|
| `hit` | Check this first. When `false`, the ray found nothing solid within reach. |
| `voxel` | The coordinates of the solid voxel that was struck. Pass this to the removal path. |
| `adjacent` | The empty cell the ray entered the hit voxel from. Pass this to the placement path. It is guaranteed empty. |
| `normal` | The face direction the ray entered from (e.g. `{0, 1, 0}` for a hit on the top face). |
| `distance` | World-space distance from the ray origin to the hit point. |

The traversal uses `WorldCoord` (double precision), not float. Cells in
non-resident chunks read as empty, so the ray passes through unstreamed regions.

---

## 4. Removing a voxel

When the player triggers removal (e.g. left-click) and the raycast hit a solid
voxel, remove it via the plugin API choke point:

```cpp
if (hit.hit) {
    WorldCoord center = chunkmath::voxelCenter(hit.voxel, world.voxelSizeM());
    Voxel empty = Voxel::empty();
    ctx->apply_edit(ctx, center, &empty);
}
```

`apply_edit` routes through the engine's single edit choke point
(`NetworkManager::applyEdit`), which:
1. Sets the voxel in the world
2. Fires all `on_voxel_modified` hooks
3. Replicates the edit to peers in multiplayer

This is the same path every local and network edit takes. Using `apply_edit`
instead of `world.setVoxel` directly ensures hooks fire, persistence works, and
multiplayer replication is correct.

### Manual path (without plugin context)

If you need direct control (e.g. in a demo's main loop without a plugin
context):

```cpp
if (hit.hit) {
    WorldCoord center = chunkmath::voxelCenter(hit.voxel, world.voxelSizeM());
    Voxel oldVox = world.getVoxel(center);
    world.setVoxel(center, Voxel::empty());

    // Fire modification hooks:
    Voxel empty = Voxel::empty();
    for (const auto& h : pluginManager.voxelModifiedHooks())
        if (h.fn) h.fn(center, &oldVox, &empty, kLocalPlayer, h.user_data);

    // Remesh the affected chunk
}
```

---

## 5. Placing a voxel

Use `hit.adjacent` -- the empty cell on the entry face -- as the placement
target:

```cpp
if (hit.hit) {
    WorldCoord placePos = chunkmath::voxelCenter(hit.adjacent, world.voxelSizeM());
    Voxel placed;
    placed.material = pluginManager.material("stone");
    ctx->apply_edit(ctx, placePos, &placed);
}
```

The placed voxel gets its full `MaterialProperties` from the material registry,
so it participates in physics, mining, structural simulation, and rendering.

---

## 6. Voxel highlight (targeting outline)

Draw a wireframe box around the targeted voxel to show the player what they are
aiming at:

```cpp
if (hit.hit) {
    renderer.drawVoxelHighlight(
        chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()),
        static_cast<float>(world.voxelSizeM()),
        0xff00ffff,    // ABGR color (yellow)
        progress       // -1.0f = no progress bar, 0.0-1.0 = mining ramp
    );
}
```

The highlight is depth-tested (no depth write) and drawn as lines over the
scene.

### The progress ramp

The `progress` parameter controls a visual mining cue:

- **-1.0** (default): plain outline at the given color, no progress shown.
- **0.0 to 1.0**: the outline ramps from the given color toward red as removal
  work accrues. A harder material takes longer to fill the ramp.

---

## 7. Mining with RemovalAccumulator

For held-to-mine interaction (hold left mouse to progressively break a voxel),
use `sim::RemovalAccumulator` from `src/simulation/RemovalAccumulator.h`:

```cpp
#include "simulation/RemovalAccumulator.h"

sim::RemovalAccumulator remover;

// Each frame while the remove action is held:
if (hit.hit) {
    float hardness = world.getVoxel(
        chunkmath::voxelCenter(hit.voxel, world.voxelSizeM())
    ).material.hardness;

    if (remover.accrue(hit.voxel, hardness, kToolPower, dt)) {
        // The voxel broke -- clear it
        WorldCoord center = chunkmath::voxelCenter(hit.voxel, world.voxelSizeM());
        Voxel empty = Voxel::empty();
        ctx->apply_edit(ctx, center, &empty);
        remover.reset();
    }

    // Visual feedback
    float progress = remover.progress();   // [0, 1]
    renderer.drawVoxelHighlight(
        chunkmath::voxelCenter(hit.voxel, world.voxelSizeM()),
        static_cast<float>(world.voxelSizeM()),
        0xff00ffff,
        progress
    );
} else {
    remover.reset();
}
```

The accumulator is property-driven: it reads `hardness` from the target voxel's
`MaterialProperties`, never a material ID. An indestructible target
(`hardness < 0`) or a powerless tool (`power <= 0`) never accrues progress.

When the targeted voxel changes, the accumulator automatically resets progress
to zero.

---

## 8. Mining laser visualization

Combine the raycast with the per-frame `drawVoxel` API to render a visible beam
from the player to the targeted voxel.

### The exercise

Walk the ray from the camera origin to `hit.distance` in small steps, calling
`drawVoxel` at each interval:

```cpp
if (hit.hit) {
    double stepSize = 0.5;   // meters between beam segments
    for (double d = 0.0; d < hit.distance; d += stepSize) {
        WorldCoord stepPos(camPos.value + lookDir * d);
        renderer.drawVoxel(stepPos, 0xff0000ff);   // ABGR red beam
    }
}
```

### Key points

- **Per-frame submission:** `drawVoxel` is immediate-mode. The beam is transient
  -- it is not placed in the world grid and vanishes if you stop submitting it.
  This is the same API used to render player markers in multiplayer
  ([Tutorial 11](11-multiplayer.md)).

- **Performance:** `drawVoxel` submits one draw call per invocation. A very long
  beam with tiny steps costs many draw calls. Recommend a step size of 0.5 to
  1.0 meters. For a beam reaching 8 m at 0.5 m steps, that is 16 draw calls per
  frame -- negligible.

- **Color:** the `abgr` parameter is packed ABGR (`0xAABBGGRR`). `0xff0000ff`
  is opaque red. Use `0xff00ff00` for green, `0xffffff00` for cyan, etc.

### Extending the beam

For a tractor-beam or scanning effect, vary the color along the ray:

```cpp
for (double d = 0.0; d < hit.distance; d += stepSize) {
    float t = static_cast<float>(d / hit.distance);
    uint8_t r = static_cast<uint8_t>(255 * (1.0f - t));
    uint8_t b = static_cast<uint8_t>(255 * t);
    uint32_t color = 0xff000000 | (b << 16) | r;   // red-to-blue gradient
    WorldCoord stepPos(camPos.value + lookDir * d);
    renderer.drawVoxel(stepPos, color);
}
```

---

## How to verify

Build and run the build/break demo:

```bash
cmake -B build && cmake --build build
./build/04-build-break-persist
```

In this demo:

- **Left-click** removes the targeted voxel (with held-to-mine feedback if
  the demo uses `RemovalAccumulator`).
- **Right-click** places a voxel on the adjacent face.
- A wireframe highlight shows which voxel you are aiming at.
- Press **G** to toggle between fly-camera and walk-mode.
- Edits persist across sessions (saved to a chunk file).

Verify that:
1. The highlight tracks the voxel under your crosshair.
2. Removing a voxel clears it and remeshes the chunk.
3. Placing a voxel fills the adjacent cell.
4. Walking mode collides with solid voxels (you cannot walk through them).

---

## Key references

| What | Where |
|------|-------|
| Raycast API and RayHit struct | `src/world/VoxelRaycast.h` |
| AABB collision (moveAABB, MoveResult) | `src/world/VoxelCollision.h` |
| RemovalAccumulator (held-to-mine) | `src/simulation/RemovalAccumulator.h` |
| drawVoxel (immediate-mode voxel) | [`include/renderer/Renderer.h`](../../include/renderer/Renderer.h) |
| drawVoxelHighlight (targeting outline) | `src/renderer/BgfxRenderer.h` |
| apply_edit (plugin edit choke point) | [`include/plugin_api.h`](../../include/plugin_api.h) |
| Camera position and rotation | [`include/renderer/Renderer.h`](../../include/renderer/Renderer.h) (`setCameraPosition`, `setCameraRotation`) |
| Build/break demo | `demos/04-build-break-persist/main.cpp` |
| Architecture: floating-origin camera | [`docs/architecture.md`](../architecture.md) section 9 |

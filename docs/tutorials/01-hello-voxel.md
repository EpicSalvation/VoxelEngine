# Tutorial 01: Hello Voxel

Build the engine from source, run your first demo, and understand the minimal
code that puts a single voxel on screen.

---

## Prerequisites

- A C++17 compiler (GCC 10+, Clang 12+, or MSVC 2019+)
- CMake 3.20 or later
- Git
- A desktop GPU with OpenGL 3.3 or Metal support

---

## 1. Clone and build

```bash
git clone <repo-url> VoxelEngine
cd VoxelEngine
cmake -B build
cmake --build build
```

The build produces:

- `build/01-single-voxel` -- the demo you will run in this tutorial
- `build/plugins/` -- shared-library plugins discovered automatically at build
  time (no CMake edits needed)
- The engine static library and test binaries

---

## 2. Run the demo

```bash
./build/01-single-voxel
```

You should see a window titled *VoxelEngine* showing a single green cube
orbiting in front of the camera. The cube is one voxel at the world origin
rendered with per-face brightness shading (top brightest, bottom darkest).

### Fly-camera controls

The demo starts in **auto-orbit** mode. Press **F** to toggle into free-camera
mode:

| Key | Action |
|-----|--------|
| **W / A / S / D** | Move forward / left / backward / right |
| **Space** | Move up |
| **Left Shift** | Move down |
| **Mouse** | Look around (pitch clamped to +/-1.55 rad) |
| **F** | Toggle auto-orbit / free-camera |
| **Escape** | Quit |

---

## 3. Anatomy of a minimal demo

Open `demos/01-single-voxel/main.cpp`. The file follows a five-step pattern
that every demo in the engine repeats.

### Step 1: Load the layer configuration

```cpp
LayerConfig layerConfig = LayerConfig::loadFromString(R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
)");
```

A `LayerConfig` describes the world's layer stack. This single-layer config
says: "one layer called `terrain`, where each voxel is a 1-meter cube, and the
layer is `terminal`" (meaning it is the editable leaf -- no further
decomposition).

### Step 2: Load plugins and start the engine

```cpp
PluginManager pluginManager;
pluginManager.loadPluginsFromDirectory("plugins");
pluginManager.wireInPlugin(voxel_plugin_init);

Engine engine;
engine.start();
```

`loadPluginsFromDirectory` scans for shared-library plugins built under
`build/plugins/`. `wireInPlugin` links a compiled-in plugin directly (the
example plugin that ships with the engine). The `Engine` owns the core
lifecycle.

### Step 3: Create a window and renderer

```cpp
platform::Window window(800, 600, "VoxelEngine — Hello Voxel");

BgfxRenderer renderer;
int fbW, fbH;
window.framebufferSize(fbW, fbH);
renderer.initialize(window.nativeHandles(),
                    static_cast<uint32_t>(fbW),
                    static_cast<uint32_t>(fbH));
```

The window is a thin wrapper around GLFW. The `BgfxRenderer` initializes the
bgfx graphics backend with the window's native handles and framebuffer
dimensions.

### Step 4: Create a world and place a voxel

```cpp
World world(1, 1, 1);
Voxel v;
v.material.palette_index = 2;  // grass green
v.material.density       = 1.0f;
world.setVoxel(0, 0, 0, v);
```

`World(1, 1, 1)` creates a 1x1x1 voxel grid. Every voxel carries a
`MaterialProperties` struct -- not a block-type ID. The `palette_index` picks
a color from the 256-entry visual palette; `density` gives the voxel physical
mass. Setting density above 0 makes the voxel non-empty.

### Step 5: Run the game loop

```cpp
while (!window.shouldClose()) {
    window.pollEvents();

    renderer.setCameraPosition(camPos);
    renderer.setCameraRotation(pitch, yaw, 0.0f);
    renderer.renderWorld(world);
    renderer.render();
}
```

Each frame: poll input, update the camera, submit the world for rendering, and
present. `camPos` is a `WorldCoord` (wraps `glm::dvec3` for double-precision
world-space positions). Pitch and yaw come from mouse deltas.

### Shutdown

```cpp
renderer.shutdown();
engine.stop();
```

---

## 4. The YAML layer config format

The layer config is the blueprint for your world's scale structure. Each entry
under `layers:` accepts these fields:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | (required) | Identifies the layer; referenced by `decompose_to`. |
| `voxel_size_m` | double | (required) | Edge length of one voxel in meters. |
| `mode` | string | (required) | `terminal` (editable leaf), `composite` (lazily decomposes), or `immutable` (render/collision only). |
| `chunk_size_voxels` | int | 32 | Voxels per chunk side (the chunk is the cube of this). |
| `view_distance_chunks` | int | 8 | Load/evict radius around the camera, in chunks. |

Stack-wide validation rules enforced at startup:

1. At least one layer must be defined.
2. `voxel_size_m` values must be strictly descending across layers.
3. Adjacent layers must have integer size ratios of at least 2:1.
4. Every `composite` layer must name a `decompose_to` target that exists.

An invalid config is a hard error at startup -- the engine exits with a
descriptive message rather than producing undefined behavior at runtime. For
the full set of per-layer fields and validation rules, see
[`docs/configuration-guide.md`](../configuration-guide.md).

---

## 5. WorldCoord and the coordinate system

All world-space positions use `WorldCoord`, a type that wraps `glm::dvec3`
(64-bit double precision). This gives sub-millimeter accuracy at
solar-system scales. You construct it explicitly:

```cpp
WorldCoord camPos(0.0, 2.0, 4.0);
```

The GPU only receives 32-bit floats. Before submission, all geometry is
translated into camera-local space (the camera's `WorldCoord` is subtracted
from every vertex), so the floats that reach the shader are always small and
precise. For the design rationale, see
[`docs/architecture.md`](../architecture.md) section 1.

---

## How to verify

1. **Build and run the demo:**

   ```bash
   cmake -B build && cmake --build build
   ./build/01-single-voxel
   ```

   You should see a green voxel. Press **F** to enter free-camera mode and fly
   around it.

2. **Run the test suite:**

   ```bash
   ctest --test-dir build
   ```

   All tests should pass. If any fail, the build environment is not set up
   correctly.

---

## Key references

| What | Where |
|------|-------|
| Demo source | `demos/01-single-voxel/main.cpp` |
| Engine lifecycle | `include/core/Engine.h` |
| Layer config parsing and validation | `include/core/LayerConfig.h` |
| WorldCoord type | `include/WorldCoord.h` |
| Plugin manager | `include/core/PluginManager.h` (see [Tutorial 02](02-your-first-plugin.md)) |
| Voxel definition | `src/world/Voxel.h` |
| Window wrapper | `include/platform/Window.h` |
| Renderer | `include/renderer/BgfxRenderer.h` |
| Full configuration reference | [`docs/configuration-guide.md`](../configuration-guide.md) |
| Architecture overview | [`docs/architecture.md`](../architecture.md) |

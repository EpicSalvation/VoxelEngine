# Tutorial 13: Structural Collapse

Configure structural physics so that unsupported voxels collapse, and write
custom plugins that decide how the collapse looks -- crumbling in place,
falling debris, or scale-filtered cascades through a multi-level composite
hierarchy.

> **Experimental.** The support-flood model this tutorial teaches works well on
> the fixed, hand-built scenes below, but it is known to misbehave on large
> streamed/open worlds (see [`docs/architecture.md` §7](../architecture.md#7-upward-damage-propagation)).
> Expect the algorithm and tuning knobs to change before it's recommended for
> that case.

---

## Prerequisites

- Engine built and running ([Tutorial 01](01-hello-voxel.md))
- Comfortable writing a plugin ([Tutorial 02](02-your-first-plugin.md))
- Familiar with materials and `structural_strength` ([Tutorial 03](03-materials-and-properties.md))
- Understanding of composite vs. terminal layers and the decomposition model

---

## 1. The propagation system

The engine's `PropagationSystem` monitors the structural integrity of every
composite macro in the world. When a terminal voxel is removed or modified, the
system walks upward through the composite hierarchy, re-aggregates strength,
and runs a support-flood to determine whether each ancestor is still stable.

The key insight: **the engine detects instability but does not decide the
response.** Detection is built in; response is delegated to plugins via the
`on_structural_event` callback. This separation lets you swap between a simple
crumble effect, a physics-driven debris shower, or complete structural
immunity -- all without modifying engine code.

### PhysicsSystem setup

```cpp
auto physics = std::make_unique<sim::PhysicsSystem>(world, pluginManager);
// Ticked each frame:
physics->tick();
```

The `PhysicsSystem` wraps `PropagationSystem` and exposes it through the plugin
API.

---

## 2. How upward propagation works

When you break a block, here is what happens inside the engine:

1. **Terminal voxel removed** -- an edit via `apply_edit` clears a leaf voxel.
2. **`on_voxel_modified` fires** -- the `PropagationSystem` hooks this event
   internally via `PropagationSystem::onVoxelModified()`.
3. **Aggregate re-computation** -- the parent composite's `aggregate_strength`
   and density are recalculated as a volume-weighted average of its children.
4. **Support-flood** -- starting from anchors (immutable voxels and
   non-resident chunk boundaries), potential `1.0` floods outward. Potential
   drains by `1 / maxSpan(strength)` per hop, where `maxSpan` is
   `strength * kSupportSpanPerStrength` capped at `kMaxSupportSpan`.
5. **Instability detected** -- any macro with `support_potential <= 0` is
   unstable. The engine fires `on_structural_event`.
6. **Plugin responds** -- the callback clears, relocates, or ignores the
   unstable voxels.
7. **Cascade** -- if the response clears children, those edits fire further
   `on_voxel_modified` events, which propagate upward to grandparents, and so
   on.

This upward cascade is what makes multi-level collapse work: mining out a 1 m
terminal voxel can topple a 4 m macro three levels above it.

---

## 3. The StructuralEvent struct

Every instability notification arrives as a `StructuralEvent`:

```cpp
struct StructuralEvent {
    WorldCoord position;           // world-space center of unstable macro
    int64_t voxel_x, voxel_y, voxel_z;  // macro VoxelCoord
    const char* layer_name;        // composite layer the macro belongs to
    double voxel_size_m;           // edge length of macro
    float aggregate_strength;      // post-edit volume-weighted structural_strength
    float support_potential;       // <= 0 means unstable
    double child_voxel_size_m;     // edge length of terminal children
};
```

| Field | Meaning |
|-------|---------|
| `position` | World-space center of the unstable macro voxel. |
| `layer_name` | Which composite layer this macro belongs to. |
| `voxel_size_m` | Edge length of the macro. A 4 m macro contains 8x8x8 children of 0.5 m each. |
| `aggregate_strength` | Volume-weighted average of children's `structural_strength`. Higher means stronger. |
| `support_potential` | Result of the support-flood. Positive = supported. Zero or negative = unsupported -- collapse. |
| `child_voxel_size_m` | Edge length of the terminal (leaf) children. Useful for iterating over them in the response. |

---

## 4. Registering a structural event handler

A minimal collapse response that clears every terminal child of the unstable
macro:

```cpp
ctx->register_on_structural_event(ctx, [](const StructuralEvent* event, void* ud) {
    // event->support_potential <= 0 means unstable
    // Response: clear the macro's children
    double childSize = event->child_voxel_size_m;
    int ratio = static_cast<int>(event->voxel_size_m / childSize);
    Voxel empty = Voxel::empty();
    for (int z = 0; z < ratio; ++z)
        for (int y = 0; y < ratio; ++y)
            for (int x = 0; x < ratio; ++x) {
                WorldCoord childPos(
                    event->position.value.x - event->voxel_size_m * 0.5 + (x + 0.5) * childSize,
                    event->position.value.y - event->voxel_size_m * 0.5 + (y + 0.5) * childSize,
                    event->position.value.z - event->voxel_size_m * 0.5 + (z + 0.5) * childSize);
                ctx->apply_edit(ctx, childPos, &empty);
            }
}, nullptr);
```

The nested loops walk every terminal child position inside the macro's bounding
cube and clear it. Each `apply_edit` may trigger further structural events
upward -- this is the cascade mechanism.

---

## 5. Immutable boundaries

Propagation stops at immutable layers. Immutable voxels (typically bedrock or
world-border layers) serve as permanent anchors for the support flood. They
emit a support potential of `1.0` and never become unstable themselves.

This means:

- A column of voxels resting on bedrock is fully supported.
- A floating island with no connection to any immutable layer will collapse as
  soon as you break one block (assuming the support-flood cannot reach an
  anchor within `kMaxSupportSpan` hops).
- Non-resident chunk boundaries also act as implicit anchors, preventing
  collapse at the edge of the loaded world.

---

## 6. Reference response plugins

The engine ships two response plugins that demonstrate different collapse
styles:

### crumble

Located at `plugins/crumble/plugin.cpp`. Clears all terminal children of the
unstable macro via `apply_edit`, producing a "the block vanishes" effect.
Simple and fast.

### falling-debris

Located at `plugins/falling-debris/plugin.cpp`. Relocates material downward in
a gravity-aware fashion. Voxels "fall" from the unstable macro and stack on the
surface below. More visually interesting but heavier on edits.

Both plugins can be hot-swapped at runtime:

```cpp
if (responsePlugin != kInvalidPluginId)
    pluginManager.unloadPlugin(responsePlugin);
responsePlugin = pluginManager.loadPlugin(crumblePath);
```

This lets you switch collapse behavior without restarting the engine -- useful
for gameplay modes or debug toggling.

---

## 7. Multi-level collapse cascades

Demo 19 (`19-multilevel-collapse`) demonstrates a deep composite stack:

```
macro (4 m) -> micro (2 m) -> grid (1 m terminal) + bedrock (0.5 m immutable)
```

Mining a 1 m grid voxel can propagate instability upward through micro and
macro layers. The cascade works because each cleared child triggers
`on_voxel_modified` on its parent, which re-aggregates and re-floods.

### Scale-filtered response

In a multi-level world you often want different responses at different scales.
For example, only collapse at the coarsest (grandparent) scale and let
intermediate levels absorb small edits:

```cpp
void grandparentCrumble(const StructuralEvent* ev, void* ud) {
    if (std::llround(ev->voxel_size_m) != 4) return;  // only 4 m macros
    // Clear all grid grandchildren...
}
```

By filtering on `voxel_size_m`, the same callback ignores instability events
at the 2 m micro scale and only fires the dramatic collapse when the 4 m macro
itself becomes unstable.

---

## 8. In-process plugin registration

For demos and tests that do not use shared-library plugins, you can register a
structural event handler using the `wireInPlugin` pattern:

```cpp
int myCollapseInit(PluginContext* ctx) {
    ctx->register_on_structural_event(ctx, myHandler, nullptr);
    return 0;
}
pluginManager.wireInPlugin(myCollapseInit);
```

This wires the handler directly into the plugin manager without loading a
`.so`/`.dll`. See [Tutorial 02](02-your-first-plugin.md) for the full plugin
lifecycle.

---

## 9. PropagationSystem internals

For advanced use (diagnostics, custom editors), the `PropagationSystem` exposes
several query functions:

| Function | Purpose |
|----------|---------|
| `active()` | Returns `true` when at least one composite-to-child level exists. |
| `levelCount()` | Number of composite levels in the hierarchy. |
| `recomputeAggregate(level, macroVoxelCoord)` | Force a full O(ratio^3) re-summation of a macro's children. |
| `aggregateStrength(level, macroVoxelCoord)` | Read the current volume-weighted average strength. |

These are engine internals, not part of the plugin API. Use them from demo code
or engine-level tooling.

---

## 10. Tuning constants

Structural physics behavior is governed by constants in `Tuning.h` and runtime
budgets in `EngineConfig`:

### Tuning.h (compile-time)

| Constant | Default | Purpose |
|----------|---------|---------|
| `tuning::physics::kSupportSpanPerStrength` | 5.0 | Voxels of support reach per unit of `structural_strength`. A material with strength 2.0 supports up to 10 voxels horizontally. |
| `tuning::physics::kMaxSupportSpan` | 16 | Absolute cap on support reach regardless of strength. |

### EngineConfig (runtime)

| Knob | Default | Purpose |
|------|---------|---------|
| `physicsMaxStructuralEventsPerFrame` | 256 | Cap on structural events fired per frame. |
| `physicsMaxSupportFloodNodes` | 4096 | Cap on nodes visited during support-flood per frame. |
| `physicsMaxAggregateRecomputesPerFrame` | 64 | Cap on aggregate re-summations per frame. |

Overflow carries to the next frame -- work is deferred, never lost. For a
complete guide to budget tuning, see
[Tutorial 14: Performance Tuning](14-performance-tuning.md).

The `structural_strength` field in `MaterialProperties` is the key material
property for structural behavior. Higher values mean a wider support span and
more stable structures. See
[Tutorial 03](03-materials-and-properties.md) for how to define materials with
different strength values.

---

## Challenge: find the support limit, then restyle the collapse

Probe the support model and swap the response.

1. In `13-structural-collapse`, build a horizontal overhang one voxel at a time,
   extending it past `kSupportSpanPerStrength x strength` voxels. It should
   collapse once the reach limit is crossed.
2. Raise the bridge material's `structural_strength` and confirm the overhang
   now reaches farther before collapsing.
3. Hot-swap the response plugin from `crumble` to `falling-debris` and repeat to
   compare the two collapse styles.

<details>
<summary>Stuck? Where to look</summary>

- Demos: `demos/13-structural-collapse/main.cpp` and
  `demos/19-multilevel-collapse/main.cpp`.
- Support reach is governed by `kSupportSpanPerStrength` / `kMaxSupportSpan`
  (section 10, `src/core/Tuning.h`).
- Swap responses between `plugins/crumble` and `plugins/falling-debris`
  (section 6); filter by `voxel_size_m` (section 7).

</details>

**Going further:** in `19-multilevel-collapse`, add a `voxel_size_m` filter to
your handler (section 7) so only the coarsest macros collapse while intermediate
levels absorb small edits.

---

## How to verify

1. **Build and run the structural-collapse demo:**

   ```bash
   cmake -B build && cmake --build build
   ./build/13-structural-collapse
   ```

   Mine out blocks from a supported structure. When enough material is removed,
   the unsupported portion collapses. Try building a horizontal overhang and
   extending it until it exceeds the support span -- it should collapse when
   the reach limit is crossed.

2. **Build and run the multi-level collapse demo:**

   ```bash
   ./build/19-multilevel-collapse
   ```

   This demo uses a deep composite hierarchy. Mine terminal voxels at the
   base and watch the collapse cascade upward through multiple composite
   levels. Note that the response only fires at the coarsest scale (4 m
   macros) -- intermediate levels absorb small edits without collapsing.

3. **Swap response plugins at runtime:** In either demo, use the plugin
   hot-swap mechanism to switch between `crumble` and `falling-debris` and
   observe the different collapse styles.

---

## Key references

| What | Where |
|------|-------|
| StructuralEvent struct | [`include/plugin_api.h`](../../include/plugin_api.h) |
| MaterialProperties (structural_strength) | [`include/plugin_api.h`](../../include/plugin_api.h) |
| PropagationSystem | `src/sim/PropagationSystem.h` |
| PhysicsSystem | `src/sim/PhysicsSystem.h` |
| crumble plugin | `plugins/crumble/plugin.cpp` |
| falling-debris plugin | `plugins/falling-debris/plugin.cpp` |
| Structural-collapse demo | `demos/13-structural-collapse/main.cpp` |
| Multi-level collapse demo | `demos/19-multilevel-collapse/main.cpp` |
| Tuning constants (support span, max span) | `src/core/Tuning.h` |
| EngineConfig (physics budgets) | [`include/core/EngineConfig.h`](../../include/core/EngineConfig.h) |
| Architecture: structural propagation | [`docs/architecture.md`](../architecture.md) |
| Materials tutorial | [Tutorial 03](03-materials-and-properties.md) |
| Performance tuning | [Tutorial 14](14-performance-tuning.md) |

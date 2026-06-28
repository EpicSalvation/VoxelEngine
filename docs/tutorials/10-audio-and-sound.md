# Tutorial 10: Audio and Sound

Add material-reactive positional audio to your game using the AudioManager
subsystem, sound registration, material-sound bindings, and persistent
emitters.

---

## Prerequisites

- The engine builds and runs successfully (see
  [Tutorial 01](01-hello-voxel.md))
- Familiarity with the plugin system ([Tutorial 02](02-your-first-plugin.md))
- Understanding of materials and palette indices
  ([Tutorial 03](03-materials-and-properties.md))
- Knowledge of the `on_voxel_modified` callback
  ([Tutorial 09](09-player-mechanics.md))

---

## 1. Audio subsystem setup

The audio subsystem is built on a `MiniaudioBackend` and managed through the
`AudioManager`. Wire it up during engine initialization:

```cpp
audio::MiniaudioBackend backend(/*useNullDevice=*/false);
audio::AudioManager audioManager(&backend, pluginManager);
pluginManager.setAudioManager(&audioManager);
audioManager.preloadSounds();
```

Pass `true` to `useNullDevice` for headless testing or CI environments where
no audio device is available.

### Listener placement

Every frame, update the listener position and orientation so the audio engine
can compute spatialization:

```cpp
glm::vec3 fwd{cos(pitch) * sin(yaw), sin(pitch), cos(pitch) * cos(yaw)};
audioManager.setListener(camPos, fwd, glm::vec3(0, 1, 0));
```

The three arguments are: world position, forward direction, and up vector.

### Audio tick

Call `update()` once per frame to pump the audio engine:

```cpp
audioManager.update();
```

---

## 2. Registering sound assets

Sounds are registered by name with a `SoundParams` struct that controls
volume and spatial attenuation:

```cpp
SoundParams blockParams{};
blockParams.volume       = 0.8f;
blockParams.attenuation  = AttenuationModel::Inverse;
blockParams.min_distance = 1.0f;
blockParams.max_distance = 40.0f;
blockParams.rolloff      = 1.0f;
blockParams.doppler      = 0.0f;  // 0 = off

ctx->register_sound(ctx, "stone_break", "assets/audio/stone_break.wav", blockParams);
```

### SoundParams fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `volume` | `float` | 1.0 | Playback volume multiplier |
| `attenuation` | `AttenuationModel` | `Inverse` | Distance falloff curve |
| `min_distance` | `float` | 1.0 | Distance at which attenuation begins |
| `max_distance` | `float` | 100.0 | Distance beyond which the sound is silent |
| `rolloff` | `float` | 1.0 | Steepness of the attenuation curve |
| `doppler` | `float` | 0.0 | Doppler factor (0 disables) |

### AttenuationModel enum

| Value | Behavior |
|-------|----------|
| `Inverse` | 1/distance falloff (realistic for most effects) |
| `Linear` | Linear ramp from min to max distance |
| `Exponential` | Steep exponential falloff |
| `None` | No distance attenuation (UI sounds, music) |

### Asset path resolution

Sound files are loaded from `assets/audio/` relative to the working
directory. The engine supports WAV format. Organize your sounds by category:

```
assets/audio/
  stone_break.wav
  stone_place.wav
  stone_step.wav
  dirt_break.wav
  dirt_place.wav
  dirt_step.wav
  ambient_wind.wav
```

---

## 3. Material-sound bindings

Rather than manually choosing a sound for every voxel interaction, bind
sounds to materials by event type. The engine resolves the correct sound at
play time based on the voxel's `palette_index`:

```cpp
ctx->register_material_sound(ctx, "stone", AudioEvent::Break,    "stone_break");
ctx->register_material_sound(ctx, "stone", AudioEvent::Place,    "stone_place");
ctx->register_material_sound(ctx, "stone", AudioEvent::Footstep, "stone_step");
```

### AudioEvent enum

| Value | When it fires |
|-------|---------------|
| `Footstep` | Player walks on this material |
| `Break` | A voxel of this material is removed |
| `Place` | A voxel of this material is placed |

This binding system means adding a new material automatically gets audio
support -- just register its sounds and bindings in your plugin's init.

---

## 4. Playing sounds

### Via material binding (preferred)

When a game event occurs, play the sound associated with the material:

```cpp
ctx->play_material_sound(ctx, AudioEvent::Break,
                         voxel.material.palette_index, position);
```

The engine looks up the material name from `palette_index`, finds the
registered sound for the given `AudioEvent`, and plays it at `position` with
the `SoundParams` it was registered with.

### Direct sound by ID

For non-material sounds (UI feedback, ambient effects), play by sound name:

```cpp
SoundParams uiParams{};
uiParams.attenuation = AttenuationModel::None;  // no distance falloff
ctx->play_sound(ctx, "ui_click", position, uiParams);
```

---

## 5. Persistent emitters

For looping or long-running sounds (ambient wind, machinery, water flow),
create a persistent emitter:

```cpp
EmitterParams ep{};
ep.sound.volume       = 0.5f;
ep.sound.min_distance = 8.0f;
ep.sound.max_distance = 80.0f;
ep.loop               = true;

AudioEmitterId emitter = ctx->create_emitter(ctx, "ambient_wind", position, ep);
```

The return value is an `AudioEmitterId`. A failed creation returns
`kInvalidEmitterId` (0).

### Updating and stopping

Move the emitter each frame if its source is dynamic:

```cpp
ctx->set_emitter_position(ctx, emitter, newPosition);
```

When the sound should stop:

```cpp
ctx->stop_emitter(ctx, emitter);
```

`stop_emitter` both stops playback and destroys the emitter handle.

### Owner-tracked resources

All sounds and emitters registered by a plugin are automatically torn down
when the plugin is unloaded. You do not need to manually clean up in a
shutdown hook -- the engine tracks ownership by plugin ID.

---

## 6. The material-audio plugin

The `material-audio` plugin (`plugins/material-audio/plugin.cpp`) is a
reference implementation that wires break/place sounds to the
`on_voxel_modified` callback:

```cpp
// In plugin init:
static PluginContext* g_ctx = nullptr;
g_ctx = ctx;

ctx->register_on_voxel_modified(ctx, [](WorldCoord pos, const Voxel* old_v,
    const Voxel* new_v, PlayerId source, void* ud) {

    if (new_v->isEmpty()) {
        // Voxel was broken
        g_ctx->play_material_sound(g_ctx, AudioEvent::Break,
                                   old_v->material.palette_index, pos);
    } else if (old_v->isEmpty()) {
        // Voxel was placed
        g_ctx->play_material_sound(g_ctx, AudioEvent::Place,
                                   new_v->material.palette_index, pos);
    }
}, nullptr);
```

The plugin stores `g_ctx` at init time so the callback closure can call back
into the plugin API. It checks `Voxel::isEmpty()` to distinguish break
(old was solid, new is empty) from place (old was empty, new is solid).

---

## 7. Footstep sounds

Footsteps require knowing which material the player is standing on. The
pattern from demo 12:

```cpp
const double kStrideLength = 2.5;  // meters per footstep
double strideAccum = 0.0;

// Each frame, while grounded and moving:
if (grounded && speed > 0.1) {
    strideAccum += speed * dt;
    if (strideAccum >= kStrideLength) {
        strideAccum -= kStrideLength;

        // Sample the voxel under the player's feet:
        WorldCoord feetPos(st->center.value - glm::dvec3(0, desc.half_y, 0));
        Voxel ground = world.getVoxel(WorldCoord(feetPos));

        audioManager.playMaterialSound(AudioEvent::Footstep,
                                       ground.material.palette_index,
                                       WorldCoord(feetPos));
    }
}
```

The stride accumulator ensures footsteps fire at a natural walking cadence
regardless of frame rate. Adjust `kStrideLength` to taste -- shorter values
give a faster-paced sound, longer values a more relaxed gait.

---

## 8. Teardown order

When shutting down, follow this order to avoid dangling references:

```cpp
// 1. Stop all emitters (automatic if plugins are unloaded first)
// 2. Disconnect the audio manager from the plugin manager:
pluginManager.setAudioManager(nullptr);
// 3. Destroy audio objects (AudioManager destructor)
```

The `AudioManager` destructor releases all backend resources. If plugins are
unloaded before the audio manager is destroyed, their emitters are already
stopped via owner tracking.

---

## Challenge: give a new material its own voice

Exercise material-sound bindings and a persistent emitter.

1. Register break/place/footstep sounds for a material you add (reuse existing
   WAVs if you don't have new ones) and bind them with
   `register_material_sound`.
2. Place that material in the world, then mine and walk on it to hear the
   bindings resolve by `palette_index`.
3. Add a looping `create_emitter` at a fixed point (e.g. a "machine hum") and
   walk toward and away from it to hear the spatial attenuation.

<details>
<summary>Stuck? Where to look</summary>

- Reference plugin: `plugins/material-audio/plugin.cpp`; demo
  `demos/12-soundscape/main.cpp`.
- Bind sounds with `register_material_sound` (section 3); loop one with
  `create_emitter` / `stop_emitter` (section 5).
- Bindings resolve by `palette_index` at play time (section 4).

</details>

**Going further:** stop the emitter with `stop_emitter` on a key press, then
confirm owner-tracking also tears it down automatically if you unload the
plugin.

---

## How to verify

Build and run the soundscape demo:

```bash
cmake -B build && cmake --build build
./build/12-soundscape
```

You should hear:

- **Footsteps** that change sound based on the ground material (stone vs.
  dirt vs. grass) as you walk.
- **Break sounds** when you mine a voxel -- stone cracks, dirt thuds.
- **Place sounds** when you build.
- **Spatial audio**: sounds are louder when close, quieter when far away,
  and panned to the correct direction.
- **Ambient emitters**: a looping wind sound in the background, correctly
  spatialized as you move around.

---

## Key references

| What | Where |
|------|-------|
| Sound registration API | `include/plugin_api.h` (`SoundParams`, `register_sound`) |
| Material-sound bindings | `include/plugin_api.h` (`register_material_sound`, `AudioEvent`) |
| Emitter API | `include/plugin_api.h` (`EmitterParams`, `create_emitter`, `stop_emitter`) |
| Material-audio plugin | `plugins/material-audio/plugin.cpp` |
| Demo source | `demos/12-soundscape/main.cpp` |
| Material properties | [Tutorial 03](03-materials-and-properties.md) |
| Voxel modification callback | [Tutorial 09](09-player-mechanics.md) |
| Architecture overview | [`docs/architecture.md`](../architecture.md) |

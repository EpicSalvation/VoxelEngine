#pragma once

// AudioManager — owns the IAudioBackend, the listener state, and the live
// emitter set. Each tick it converts every emitter's WorldCoord to camera-local
// float (the floating-origin projection from ARCHITECTURE §1/§9/§16) and submits
// to the backend.
//
// Dependency rules (ARCHITECTURE §13/§16):
//   Depends on: PluginManager (sound + material-sound registries), WorldCoord, IAudioBackend
//   Must NOT depend on: Renderer, World, PhysicsSystem, DecompositionWorker, IO

#include "audio/IAudioBackend.h"
#include "WorldCoord.h"
#include "plugin_api.h"
#include "core/PluginManager.h"   // PluginId, RegisteredSound, RegisteredMaterialSound
#include <string>
#include <unordered_map>

namespace audio {

class AudioManager {
public:
    AudioManager(IAudioBackend* backend, PluginManager& pm);

    // Called once after plugins load: loads every registered sound asset into
    // the backend. Missing assets are reported via std::cerr (fail-soft in
    // release; see ARCHITECTURE §16 on the validateAudio policy pass).
    void preloadSounds();

    // Push listener state each frame. pos is the camera WorldCoord; all emitter
    // positions are re-projected relative to it this tick.
    void setListener(const WorldCoord& pos, const glm::vec3& forward, const glm::vec3& up);

    // One-shot positional playback. Fail-soft when sound_id is unknown.
    void playSound(const std::string& sound_id, const WorldCoord& pos,
                   const SoundParams* overrides = nullptr);

    // Resolve (AudioEvent, palette_index) → sound_id via the material-sound registry
    // and play. Fail-soft when the binding is not found (no sound, no throw).
    void playMaterialSound(AudioEvent event, uint8_t palette_index, const WorldCoord& pos);

    // Persistent emitter lifecycle. owner tags the emitter so it can be stopped
    // when the owning plugin unloads (pass kInvalidPluginId for demo-owned emitters).
    AudioEmitterId createEmitter(const std::string& sound_id, const WorldCoord& pos,
                                  const EmitterParams& params,
                                  PluginId owner = kInvalidPluginId);
    void setEmitterPosition(AudioEmitterId id, const WorldCoord& pos);
    void stopEmitter(AudioEmitterId id);

    // Stop every emitter owned by plugin `owner`. Called by PluginManager::unloadPlugin.
    void stopEmittersOwnedBy(PluginId owner);

    // Tick: re-project emitter positions to camera-local float, push to backend,
    // let backend prune finished one-shots.
    void update();

private:
    glm::vec3 toLocal(const WorldCoord& pos) const;

    IAudioBackend* backend_;
    PluginManager& pm_;
    WorldCoord     listenerPos_{};

    struct LiveEmitter {
        WorldCoord     worldPos;
        AudioEmitterId backendId;
        PluginId       owner;
    };
    std::unordered_map<AudioEmitterId, LiveEmitter> emitters_;
    AudioEmitterId nextId_ = 1;  // 0 == kInvalidEmitterId
};

} // namespace audio

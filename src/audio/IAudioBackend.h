#pragma once

// Abstract audio backend seam (ARCHITECTURE §16).
// No miniaudio type appears here; this is the boundary a plugin replaces to
// swap MiniaudioBackend for FMOD, Wwise, or a custom mixer.

#include "plugin_api.h"
#include <glm/glm.hpp>
#include <string>

namespace audio {

class IAudioBackend {
public:
    virtual ~IAudioBackend();

    // Register a sound asset by id. Returns true when the asset is reachable
    // and the backend accepts it; false on failure (caller logs/validates).
    // Called from AudioManager::preloadSounds() after plugins load.
    virtual bool loadSound(const std::string& sound_id,
                           const std::string& path,
                           const SoundParams& params) = 0;

    // Fire-and-forget one-shot at a camera-local position.
    // local_pos is already WorldCoord::toLocalFloat(camera) — never world-absolute.
    // params may be nullptr to use the defaults registered with loadSound.
    // Fail-soft: logs nothing when sound_id is unregistered.
    virtual void playOneShot(const std::string& sound_id,
                             const glm::vec3&   local_pos,
                             const SoundParams* params) = 0;

    // Create a persistent looping emitter at a camera-local position.
    // Returns kInvalidEmitterId on failure (unregistered sound, backend error).
    virtual AudioEmitterId createEmitter(const std::string&  sound_id,
                                         const glm::vec3&    local_pos,
                                         const EmitterParams& params) = 0;

    // Re-project an emitter's position each tick. Safe to call every frame.
    virtual void setEmitterPosition(AudioEmitterId id, const glm::vec3& local_pos) = 0;

    // Stop and destroy an emitter. Idempotent for kInvalidEmitterId.
    virtual void stopEmitter(AudioEmitterId id) = 0;

    // Push listener orientation. Position is always the local origin (floating-origin
    // rule: the listener is pinned at (0,0,0) and emitters are fed camera-relative
    // floats — see ARCHITECTURE §1/§9/§16).
    virtual void setListener(const glm::vec3& forward, const glm::vec3& up) = 0;

    // Tick the backend: prune finished one-shots, flush any pending state.
    virtual void update() = 0;

    // Shut down the backend and release all resources.
    virtual void shutdown() = 0;

    // True when the backend initialized successfully.
    virtual bool isReady() const = 0;
};

} // namespace audio

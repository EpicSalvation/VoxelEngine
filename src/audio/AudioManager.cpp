#include "audio/AudioManager.h"
#include "core/PluginManager.h"
#include <algorithm>
#include <iostream>
#include <vector>

namespace audio {

AudioManager::AudioManager(IAudioBackend* backend, PluginManager& pm)
    : backend_(backend), pm_(pm) {}

glm::vec3 AudioManager::toLocal(const WorldCoord& pos) const {
    return pos.toLocalFloat(listenerPos_);
}

void AudioManager::preloadSounds() {
    if (!backend_ || !backend_->isReady()) return;
    for (const auto& s : pm_.sounds()) {
        if (!backend_->loadSound(s.sound_id, s.path, s.params))
            std::cerr << "[AudioManager] Could not load sound '" << s.sound_id
                      << "' from '" << s.path << "'\n";
    }
}

void AudioManager::setListener(const WorldCoord& pos,
                                const glm::vec3&  forward,
                                const glm::vec3&  up) {
    listenerPos_ = pos;
    if (backend_) backend_->setListener(forward, up);
}

void AudioManager::playSound(const std::string& sound_id,
                              const WorldCoord&  pos,
                              const SoundParams* overrides) {
    if (!backend_ || !backend_->isReady()) return;
    // Convert to camera-local float once: used for the cull check and submitted
    // to the backend. World-absolute floats are never given to the audio engine
    // (the same floating-origin rule as the GPU path, ARCHITECTURE §1/§9/§16).
    glm::vec3 local = toLocal(pos);

    // Pre-cull: skip voice allocation for sources beyond their max audible distance.
    // Prevents distant voxel events (remote edits, off-screen collapses) from
    // accumulating inaudible voices (ARCHITECTURE §16).
    float maxDist = overrides ? overrides->max_distance
                              : [&]() -> float {
                                    const auto* r = pm_.findSound(sound_id);
                                    return r ? r->params.max_distance : SoundParams{}.max_distance;
                                }();
    float distSq = local.x*local.x + local.y*local.y + local.z*local.z;
    if (distSq > maxDist * maxDist) return;

    backend_->playOneShot(sound_id, local, overrides);
}

void AudioManager::playMaterialSound(AudioEvent event,
                                      uint8_t    palette_index,
                                      const WorldCoord& pos) {
    if (!backend_ || !backend_->isReady()) return;
    const auto* binding = pm_.findMaterialSound(event, palette_index);
    if (!binding) return;  // fail-soft: unbound material/event pair

    glm::vec3 local = toLocal(pos);

    // Pre-cull using the bound sound's registered max_distance.
    float maxDist = SoundParams{}.max_distance;
    if (const auto* reg = pm_.findSound(binding->sound_id))
        maxDist = reg->params.max_distance;
    float distSq = local.x*local.x + local.y*local.y + local.z*local.z;
    if (distSq > maxDist * maxDist) return;

    backend_->playOneShot(binding->sound_id, local, nullptr);
}

AudioEmitterId AudioManager::createEmitter(const std::string&  sound_id,
                                            const WorldCoord&   pos,
                                            const EmitterParams& params,
                                            PluginId             owner) {
    if (!backend_ || !backend_->isReady()) return kInvalidEmitterId;
    AudioEmitterId bid = backend_->createEmitter(sound_id, toLocal(pos), params);
    if (bid == kInvalidEmitterId) return kInvalidEmitterId;

    AudioEmitterId id = nextId_++;
    emitters_[id] = {pos, bid, owner};
    return id;
}

void AudioManager::setEmitterPosition(AudioEmitterId id, const WorldCoord& pos) {
    auto it = emitters_.find(id);
    if (it == emitters_.end()) return;
    it->second.worldPos = pos;
    if (backend_) backend_->setEmitterPosition(it->second.backendId, toLocal(pos));
}

void AudioManager::stopEmitter(AudioEmitterId id) {
    auto it = emitters_.find(id);
    if (it == emitters_.end()) return;
    if (backend_) backend_->stopEmitter(it->second.backendId);
    emitters_.erase(it);
}

void AudioManager::stopEmittersOwnedBy(PluginId owner) {
    std::vector<AudioEmitterId> toStop;
    for (const auto& [id, e] : emitters_)
        if (e.owner == owner)
            toStop.push_back(id);
    for (AudioEmitterId id : toStop)
        stopEmitter(id);
}

void AudioManager::update() {
    if (!backend_ || !backend_->isReady()) return;
    // Re-project every live emitter to the current camera-local frame.
    for (auto& [id, e] : emitters_)
        backend_->setEmitterPosition(e.backendId, toLocal(e.worldPos));
    backend_->update();
}

size_t AudioManager::activeVoiceCount() const {
    return backend_ ? backend_->activeVoiceCount() : 0;
}

} // namespace audio

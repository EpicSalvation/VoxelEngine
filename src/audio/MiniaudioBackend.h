#pragma once

// MiniaudioBackend — the ONLY engine file that includes miniaudio.h.
// All other code interacts with audio through IAudioBackend.
// See ARCHITECTURE §16 for the adapter rationale.

#include "audio/IAudioBackend.h"
#include <memory>

namespace audio {

class MiniaudioBackend : public IAudioBackend {
public:
    // useNullDevice = true: initialise without a real audio device (for tests
    // and headless CI). Sounds load via the resource manager and voice state is
    // tracked correctly; no audio is output to hardware.
    explicit MiniaudioBackend(bool useNullDevice = false);
    ~MiniaudioBackend() override;

    bool loadSound(const std::string& sound_id,
                   const std::string& path,
                   const SoundParams& params) override;

    void playOneShot(const std::string& sound_id,
                     const glm::vec3&   local_pos,
                     const SoundParams* params) override;

    AudioEmitterId createEmitter(const std::string&  sound_id,
                                  const glm::vec3&    local_pos,
                                  const EmitterParams& params) override;

    void setEmitterPosition(AudioEmitterId id, const glm::vec3& local_pos) override;

    void stopEmitter(AudioEmitterId id) override;

    void setListener(const glm::vec3& forward, const glm::vec3& up) override;

    void update() override;

    void shutdown() override;

    bool isReady() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace audio

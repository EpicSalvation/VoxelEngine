// MiniaudioBackend.cpp — the ONLY translation unit that includes miniaudio.h.
// MINIAUDIO_IMPLEMENTATION must appear before the include in exactly one .cpp.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio/MiniaudioBackend.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace audio {
namespace {

static ma_attenuation_model toMaAttenuation(AttenuationModel m) {
    switch (m) {
        case AttenuationModel::Inverse:     return ma_attenuation_model_inverse;
        case AttenuationModel::Linear:      return ma_attenuation_model_linear;
        case AttenuationModel::Exponential: return ma_attenuation_model_exponential;
        case AttenuationModel::None:        return ma_attenuation_model_none;
        default:                            return ma_attenuation_model_inverse;
    }
}

static void applySoundParams(ma_sound* snd, const SoundParams& sp) {
    ma_sound_set_volume(snd, sp.volume);
    ma_sound_set_min_distance(snd, sp.min_distance);
    ma_sound_set_max_distance(snd, sp.max_distance);
    ma_sound_set_attenuation_model(snd, toMaAttenuation(sp.attenuation));
    ma_sound_set_rolloff(snd, sp.rolloff);
    if (sp.doppler > 0.0f)
        ma_sound_set_doppler_factor(snd, sp.doppler);
}

} // namespace

struct MiniaudioBackend::Impl {
    ma_engine engine{};
    bool      useNullDevice = false;
    bool      ready         = false;

    // Per-registered-sound metadata (path + default params)
    struct SoundAsset {
        std::string path;
        SoundParams params;
    };
    std::unordered_map<std::string, SoundAsset> assets;

    // Live one-shots: fire-and-forget sounds, cleaned up in update() on completion
    std::vector<ma_sound*> oneShots;

    // Persistent emitters keyed by the opaque AudioEmitterId we issue
    std::unordered_map<AudioEmitterId, ma_sound*> emitters;
    AudioEmitterId nextEmitterId = 1;  // 0 == kInvalidEmitterId

    // Null device: miniaudio context/device so ma_backend_null can be selected
    ma_context nullContext{};
    ma_device  nullDevice{};
    bool       nullContextReady = false;
    bool       nullDeviceReady  = false;

    void pruneDoneOneShots() {
        auto it = oneShots.begin();
        while (it != oneShots.end()) {
            if (ma_sound_at_end(*it)) {
                ma_sound_uninit(*it);
                delete *it;
                it = oneShots.erase(it);
            } else {
                ++it;
            }
        }
    }

    void shutdownEmitters() {
        for (auto& [id, snd] : emitters) {
            ma_sound_uninit(snd);
            delete snd;
        }
        emitters.clear();
    }

    void shutdownOneShots() {
        for (ma_sound* snd : oneShots) {
            ma_sound_uninit(snd);
            delete snd;
        }
        oneShots.clear();
    }
};

MiniaudioBackend::MiniaudioBackend(bool useNullDevice)
    : impl_(std::make_unique<Impl>())
{
    impl_->useNullDevice = useNullDevice;

    ma_engine_config cfg = ma_engine_config_init();

    if (useNullDevice) {
        // Init a context restricted to the null backend, then a playback device
        // on that context, and hand the device to the engine. This lets the
        // engine run its audio thread against a device that discards all output,
        // so ma_sound_at_end() and other state work correctly without hardware.
        ma_backend nullBackend = ma_backend_null;
        ma_context_config ctxCfg = ma_context_config_init();
        if (ma_context_init(&nullBackend, 1, &ctxCfg, &impl_->nullContext) != MA_SUCCESS) {
            std::cerr << "[MiniaudioBackend] Failed to init null context\n";
            return;
        }
        impl_->nullContextReady = true;

        ma_device_config devCfg = ma_device_config_init(ma_device_type_playback);
        devCfg.playback.format   = ma_format_f32;
        devCfg.playback.channels = 2;
        devCfg.sampleRate        = 44100;
        devCfg.pUserData         = &impl_->engine;
        devCfg.dataCallback      = [](ma_device* pDev, void* pOut, const void*, ma_uint32 fc) {
            ma_engine_read_pcm_frames(static_cast<ma_engine*>(pDev->pUserData),
                                      pOut, fc, nullptr);
        };
        if (ma_device_init(&impl_->nullContext, &devCfg, &impl_->nullDevice) != MA_SUCCESS) {
            std::cerr << "[MiniaudioBackend] Failed to init null device\n";
            return;
        }
        impl_->nullDeviceReady = true;
        cfg.pDevice = &impl_->nullDevice;
    }

    if (ma_engine_init(&cfg, &impl_->engine) != MA_SUCCESS) {
        std::cerr << "[MiniaudioBackend] Failed to init audio engine\n";
        return;
    }

    if (useNullDevice && impl_->nullDeviceReady) {
        // The engine owns the device's callback; start it manually for the null path.
        ma_device_start(&impl_->nullDevice);
    }

    // Listener is always at the local origin (floating-origin rule, ARCHITECTURE §16).
    ma_engine_listener_set_position(&impl_->engine, 0, 0.0f, 0.0f, 0.0f);
    impl_->ready = true;
}

MiniaudioBackend::~MiniaudioBackend() {
    shutdown();
}

bool MiniaudioBackend::isReady() const {
    return impl_->ready;
}

size_t MiniaudioBackend::activeVoiceCount() const {
    // Live one-shots (not yet pruned by update()) plus persistent emitters.
    return impl_->oneShots.size() + impl_->emitters.size();
}

bool MiniaudioBackend::loadSound(const std::string& sound_id,
                                  const std::string& path,
                                  const SoundParams& params) {
    if (!impl_->ready) return false;
    // Verify the path is accessible before caching it. This is the check done
    // during the validateAudio startup pass; runtime playback is fail-soft and
    // does not re-probe (ARCHITECTURE §16 missing-sound policy).
    if (!path.empty() && !std::filesystem::exists(path)) return false;
    impl_->assets[sound_id] = {path, params};
    return true;
}

void MiniaudioBackend::playOneShot(const std::string& sound_id,
                                    const glm::vec3&   local_pos,
                                    const SoundParams* params) {
    if (!impl_->ready) return;
    auto it = impl_->assets.find(sound_id);
    if (it == impl_->assets.end()) return;  // fail-soft: unregistered sound

    auto* snd = new ma_sound{};
    // MA_SOUND_FLAG_ASYNC: the resource manager decodes/streams in background;
    // the engine uses cached decoded data for repeat plays of the same file.
    ma_result r = ma_sound_init_from_file(&impl_->engine,
                                           it->second.path.c_str(),
                                           MA_SOUND_FLAG_ASYNC,
                                           nullptr, nullptr, snd);
    if (r != MA_SUCCESS) {
        delete snd;
        return;
    }

    ma_sound_set_position(snd, local_pos.x, local_pos.y, local_pos.z);
    applySoundParams(snd, params ? *params : it->second.params);
    ma_sound_start(snd);
    impl_->oneShots.push_back(snd);
}

AudioEmitterId MiniaudioBackend::createEmitter(const std::string&   sound_id,
                                                const glm::vec3&     local_pos,
                                                const EmitterParams& params) {
    if (!impl_->ready) return kInvalidEmitterId;
    auto it = impl_->assets.find(sound_id);
    if (it == impl_->assets.end()) return kInvalidEmitterId;

    auto* snd = new ma_sound{};
    ma_result r = ma_sound_init_from_file(&impl_->engine,
                                           it->second.path.c_str(),
                                           MA_SOUND_FLAG_ASYNC,
                                           nullptr, nullptr, snd);
    if (r != MA_SUCCESS) {
        delete snd;
        return kInvalidEmitterId;
    }

    ma_sound_set_position(snd, local_pos.x, local_pos.y, local_pos.z);
    applySoundParams(snd, params.sound);
    if (params.loop) ma_sound_set_looping(snd, MA_TRUE);
    ma_sound_start(snd);

    AudioEmitterId id = impl_->nextEmitterId++;
    impl_->emitters[id] = snd;
    return id;
}

void MiniaudioBackend::setEmitterPosition(AudioEmitterId id, const glm::vec3& local_pos) {
    if (!impl_->ready) return;
    auto it = impl_->emitters.find(id);
    if (it == impl_->emitters.end()) return;
    ma_sound_set_position(it->second, local_pos.x, local_pos.y, local_pos.z);
}

void MiniaudioBackend::stopEmitter(AudioEmitterId id) {
    if (id == kInvalidEmitterId) return;
    auto it = impl_->emitters.find(id);
    if (it == impl_->emitters.end()) return;
    ma_sound_stop(it->second);
    ma_sound_uninit(it->second);
    delete it->second;
    impl_->emitters.erase(it);
}

void MiniaudioBackend::setListener(const glm::vec3& forward, const glm::vec3& up) {
    if (!impl_->ready) return;
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0, up.x, up.y, up.z);
}

void MiniaudioBackend::update() {
    if (!impl_->ready) return;
    impl_->pruneDoneOneShots();
}

void MiniaudioBackend::shutdown() {
    if (!impl_->ready) return;
    impl_->ready = false;
    impl_->shutdownOneShots();
    impl_->shutdownEmitters();
    ma_engine_uninit(&impl_->engine);
    if (impl_->nullDeviceReady) {
        ma_device_uninit(&impl_->nullDevice);
        impl_->nullDeviceReady = false;
    }
    if (impl_->nullContextReady) {
        ma_context_uninit(&impl_->nullContext);
        impl_->nullContextReady = false;
    }
}

} // namespace audio

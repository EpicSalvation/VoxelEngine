#include "audio/AudioValidation.h"
#include "audio/IAudioBackend.h"
#include "core/PluginManager.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace audio {

std::vector<AudioValidationIssue> validateAudio(const PluginManager& pm,
                                                 IAudioBackend*       backend,
                                                 AudioStrictPolicy    policy) {
    std::vector<AudioValidationIssue> issues;

    // --- Pass 1: dangling material-sound bindings ---
    // Every register_material_sound entry must resolve to a registered sound_id.
    for (const auto& ms : pm.materialSounds()) {
        if (!pm.findSound(ms.sound_id)) {
            issues.push_back({AudioValidationIssue::DanglingBinding, ms.sound_id,
                "material '" + ms.material_id + "' bound to unregistered sound_id '" +
                ms.sound_id + "'"});
        }
    }

    // --- Pass 2: unloadable sound assets ---
    // Call backend.loadSound() for each registered asset; failure means the file
    // is missing or the backend cannot decode it. Skip when backend is null.
    if (backend) {
        for (const auto& s : pm.sounds()) {
            if (!backend->loadSound(s.sound_id, s.path, s.params)) {
                issues.push_back({AudioValidationIssue::UnloadableAsset, s.sound_id,
                    "cannot load asset for sound_id '" + s.sound_id +
                    "' from path '" + s.path + "'"});
            }
        }
    }

    if (issues.empty()) return issues;

    // --- Resolve policy ---
    bool asError;
    switch (policy) {
        case AudioStrictPolicy::Error: asError = true;  break;
        case AudioStrictPolicy::Warn:  asError = false; break;
        case AudioStrictPolicy::Auto:
        default:
#ifndef NDEBUG
            asError = true;
#else
            asError = false;
#endif
            break;
    }

    // Build the consolidated message (all issues in one pass).
    std::ostringstream msg;
    msg << "[validateAudio] " << issues.size() << " problem(s) found:\n";
    for (const auto& issue : issues)
        msg << "  - " << issue.detail << "\n";

    if (asError) {
        throw std::runtime_error(msg.str());
    } else {
        std::cerr << msg.str();
    }

    return issues;
}

} // namespace audio

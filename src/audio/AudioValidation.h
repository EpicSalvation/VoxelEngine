#pragma once

// Audio startup-validation pass (ARCHITECTURE §16).
//
// validateAudio() walks every register_material_sound binding and every
// register_sound asset, collects all problems in one pass, then applies the
// severity policy:
//
//   Error — always throws std::runtime_error (CI gate, always-hard audio).
//   Warn  — logs all issues and returns (suitable for shipped games where a
//            missing footstep sound must not refuse to run).
//   Auto  — Error in debug builds (#ifndef NDEBUG), Warn in release.
//
// The policy mirrors the tri-state established by net.interest (M11) — one
// line in the project config, with a sane build-defaulted behaviour.
//
// Runtime resolution is intentionally separate and always fail-soft:
// play_sound / play_material_sound play nothing when a sound or binding is
// missing, and never throw. Only this startup pass is strict.

#include <string>
#include <vector>

class PluginManager;
namespace audio { class IAudioBackend; }

namespace audio {

enum class AudioStrictPolicy {
    Auto,   // Error in debug (#ifndef NDEBUG), Warn in release
    Error,  // Always throw std::runtime_error with all issues listed
    Warn,   // Always log and return (no throw)
};

struct AudioValidationIssue {
    enum Kind { DanglingBinding, UnloadableAsset } kind;
    std::string sound_id;  // the sound_id at the centre of the problem
    std::string detail;    // human-readable explanation
};

// Walk pm.materialSounds() (does each sound_id resolve to a registered sound?)
// and pm.sounds() (does backend.loadSound() succeed for each asset?).
// Collects ALL problems before applying the policy, so one run reveals
// every issue rather than stopping at the first.
//
// When backend is nullptr the asset-probe step is skipped (only binding
// completeness is checked — useful in contexts where no audio device exists).
//
// Returns the (possibly empty) list of issues found.
// Throws std::runtime_error (including all issue messages) when the resolved
// policy is Error.
std::vector<AudioValidationIssue> validateAudio(
    const PluginManager& pm,
    IAudioBackend*       backend,
    AudioStrictPolicy    policy = AudioStrictPolicy::Auto
);

} // namespace audio

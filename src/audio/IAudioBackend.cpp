#include "audio/IAudioBackend.h"

namespace audio {
// Out-of-line destructor anchors the vtable here, avoiding a weak-vtable
// warning on GCC/Clang when the class has no non-inline virtual functions.
IAudioBackend::~IAudioBackend() = default;
} // namespace audio

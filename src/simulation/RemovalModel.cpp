#include "simulation/RemovalModel.h"

#include <limits>

namespace sim {

float removalWork(float hardness) {
    // Indestructible: no finite work threshold. Callers gate on isRemovable
    // before consuming this; the +inf keeps an accumulator's `accrued >= work`
    // comparison correct (never satisfied) even if they don't.
    if (hardness < 0.0f) return std::numeric_limits<float>::infinity();
    // hardness == 0 -> 0 (instant); hardness > 0 -> linear in hardness.
    return hardness;
}

float removalTime(float hardness, float power) {
    if (!isRemovable(hardness, power)) {
        return std::numeric_limits<float>::infinity();
    }
    // power > 0 and hardness >= 0 here. hardness == 0 yields 0 (instant).
    return removalWork(hardness) / power;
}

}  // namespace sim

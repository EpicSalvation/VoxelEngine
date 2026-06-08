#include "simulation/RemovalAccumulator.h"

#include <algorithm>

namespace sim {

bool RemovalAccumulator::accrue(const chunkmath::VoxelCoord& target,
                                float hardness, float power, float dt) {
    // Retarget: a new target (or first contact) starts from zero progress.
    if (!has_target_ || target_ != target) {
        has_target_ = true;
        target_     = target;
        accrued_    = 0.0f;
    }

    // Cache the model's verdict for this target so progress() can report a
    // fraction without re-reading the material.
    removable_ = isRemovable(hardness, power);
    threshold_ = removalWork(hardness);

    // Indestructible target or powerless tool: never accrue, never clear.
    if (!removable_) return false;

    accrued_ += power * dt;

    // hardness == 0 -> threshold 0 -> clears on the first tick (fail-soft
    // instant). hardness > 0 -> clears once accrued work meets the threshold.
    return accrued_ >= threshold_;
}

void RemovalAccumulator::reset() {
    has_target_ = false;
    target_     = chunkmath::VoxelCoord{};
    accrued_    = 0.0f;
    threshold_  = 0.0f;
    removable_  = false;
}

float RemovalAccumulator::progress() const {
    if (!has_target_ || !removable_) return 0.0f;
    if (threshold_ <= 0.0f) return 1.0f;  // instant target: already "done"
    return std::min(accrued_ / threshold_, 1.0f);
}

}  // namespace sim

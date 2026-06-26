#include "simulation/ThermalSystem.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "core/EngineConfig.h"
#include "core/PluginManager.h"
#include "core/Tuning.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace sim {

using chunkmath::VoxelCoord;
using chunkmath::VoxelCoordHash;

namespace {
// Snap-to-ambient epsilon: explicit diffusion only asymptotically approaches
// ambient, never landing on it exactly in float, which would otherwise keep
// every decaying cell active forever. Within this band a cell is close enough
// that snapping it to ambient() (an exact match, so FieldOverlay::set drops it
// from the active set) is undetectable as a sudden jump. Not a tuning::thermal
// knob — it is an implementation epsilon, not a design choice to retune.
constexpr float kAmbientSnapEpsilon = 1e-3f;
}  // namespace

ThermalSystem::ThermalSystem(const World& world, PluginManager& pm)
    : world_(world), pm_(pm), overlay_(tuning::thermal::kAmbientTemperature) {
    terminal_ = world_.primaryLayer();
}

ThermalSystem::~ThermalSystem() = default;

float ThermalSystem::temperatureAt(const WorldCoord& pos) const {
    if (!terminal_) return overlay_.ambient();
    return overlay_.get(chunkmath::worldToVoxel(pos, terminal_->voxelSizeM()));
}

bool ThermalSystem::resident(const VoxelCoord& c) const {
    if (!terminal_) return false;
    const WorldCoord center = chunkmath::voxelCenter(c, terminal_->voxelSizeM());
    const ChunkCoord cc = chunkmath::worldToChunk(center, terminal_->voxelSizeM(),
                                                  terminal_->chunkSizeVoxels());
    return terminal_->getChunk(cc) != nullptr;
}

float ThermalSystem::conductivityAt(const VoxelCoord& c) const {
    if (!resident(c)) return 0.0f;  // outside the resident region: no diffusion
    const WorldCoord center = chunkmath::voxelCenter(c, terminal_->voxelSizeM());
    return terminal_->getVoxel(center).material.thermal_conductivity;
}

void ThermalSystem::applySources(double dt) {
    if (!terminal_) return;
    for (const auto& src : pm_.heatSources()) {
        const VoxelCoord c = chunkmath::worldToVoxel(src.pos, terminal_->voxelSizeM());
        if (!resident(c)) continue;
        relax(c, overlay_.get(c) + src.rate * static_cast<float>(dt));
    }
}

void ThermalSystem::relax(const VoxelCoord& c, float newT) {
    if (std::fabs(newT - overlay_.ambient()) < kAmbientSnapEpsilon)
        newT = overlay_.ambient();

    const bool wasActive = overlay_.isActive(c);
    overlay_.set(c, newT);
    const bool isActiveNow = overlay_.isActive(c);
    if (isActiveNow == wasActive) return;

    ThermalFieldEvent ev{};
    ev.position    = chunkmath::voxelCenter(c, terminal_->voxelSizeM());
    ev.voxel_x     = c.x;
    ev.voxel_y     = c.y;
    ev.voxel_z     = c.z;
    ev.temperature = overlay_.get(c);
    ev.crossing    = isActiveNow ? FieldCrossing::Rising : FieldCrossing::Falling;

    for (const auto& hook : pm_.thermalEventHooks())
        if (hook.fn) hook.fn(&ev, hook.user_data);
    ++eventsLastTick_;
}

void ThermalSystem::tick(double dt) {
    eventsLastTick_ = 0;
    if (!active()) return;

    applySources(dt);

    // Candidate set: the active set + its frontier, sorted (§4 determinism).
    std::vector<VoxelCoord> work = overlay_.activeSorted();
    for (const VoxelCoord& c : overlay_.frontierSorted()) work.push_back(c);
    if (work.empty()) return;

    namespace tt = tuning::thermal;
    const int maxCells = engineConfig().thermalMaxCellsPerFrame;
    if (static_cast<int>(work.size()) > maxCells)
        work.resize(maxCells);  // sorted prefix; the rest stays
                                                     // in the overlay's own active
                                                     // set/frontier for next tick

    // Sub-step count: respect the explicit-scheme stability bound for the most
    // conductive cell touched this tick (3D: k*dt/dx^2 <= kStabilityFactor, dx=1
    // cell at this granularity).
    float maxK = 0.0f;
    for (const VoxelCoord& c : work) maxK = std::max(maxK, conductivityAt(c));
    if (maxK <= 0.0f) return;  // nothing here can conduct

    const double dtMax = static_cast<double>(tt::kStabilityFactor) / maxK;
    const int substeps = std::max(1, static_cast<int>(std::ceil(dt / dtMax)));
    const double subDt = dt / substeps;

    for (int s = 0; s < substeps; ++s) {
        std::unordered_map<VoxelCoord, float, VoxelCoordHash> next;
        next.reserve(work.size());
        for (const VoxelCoord& c : work) {
            const float k = conductivityAt(c);
            const float t = overlay_.get(c);
            float laplacian = 0.0f;
            for (const VoxelCoord& n : neighbors6(c)) laplacian += overlay_.get(n) - t;
            next[c] = t + k * static_cast<float>(subDt) * laplacian;
        }
        for (const auto& [c, t] : next) relax(c, t);
    }
}

}  // namespace sim

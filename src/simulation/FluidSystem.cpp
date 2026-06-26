#include "simulation/FluidSystem.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/PluginManager.h"
#include "core/EngineConfig.h"
#include "core/Tuning.h"
#include "world/Layer.h"
#include "world/World.h"

namespace sim {

using chunkmath::VoxelCoord;
using chunkmath::VoxelCoordHash;

FluidSystem::FluidSystem(const World& world, PluginManager& pm)
    : world_(world), pm_(pm), overlay_(0.0f) {
    terminal_ = world_.primaryLayer();
    // Rides the same engine-owned choke point PhysicsSystem uses, so the
    // realized-fluid set tracks every committed edit (local or replicated)
    // without NetworkManager ever depending on the simulation tier.
    pm_.registerEngineVoxelModifiedHook(&FluidSystem::onVoxelModifiedThunk, this);
}

FluidSystem::~FluidSystem() {
    pm_.unregisterEngineVoxelModifiedHook(this);
}

float FluidSystem::amountAt(const WorldCoord& pos) const {
    if (!terminal_) return overlay_.ambient();
    return overlay_.get(chunkmath::worldToVoxel(pos, terminal_->voxelSizeM()));
}

bool FluidSystem::resident(const VoxelCoord& c) const {
    if (!terminal_) return false;
    const WorldCoord center = chunkmath::voxelCenter(c, terminal_->voxelSizeM());
    const ChunkCoord cc = chunkmath::worldToChunk(center, terminal_->voxelSizeM(),
                                                  terminal_->chunkSizeVoxels());
    return terminal_->getChunk(cc) != nullptr;
}

float FluidSystem::porosityAt(const VoxelCoord& c) const {
    // A non-resident destination blocks flow — the conservative boundary rule
    // (the fluid analog of PropagationSystem's "non-resident ⇒ anchor"): with
    // no real chunk there to validate against, fluid must not leak past the
    // resident-region edge and grow the overlay unbounded.
    if (!resident(c)) return 0.0f;
    const Voxel v = terminal_->getVoxel(chunkmath::voxelCenter(c, terminal_->voxelSizeM()));
    if (v.isEmpty()) return 1.0f;   // air: effective porosity 1.0 (§17)
    return v.material.porosity;
}

bool FluidSystem::isFluidVoxel(const Voxel& v) const {
    if (v.isEmpty()) return false;
    for (const auto& src : pm_.fluidSources())
        if (src.palette_index == v.material.palette_index) return true;
    return false;
}

void FluidSystem::onVoxelModifiedThunk(WorldCoord pos, const Voxel* oldVoxel,
                                       const Voxel* newVoxel, PlayerId /*source*/,
                                       void* user_data) {
    auto* self = static_cast<FluidSystem*>(user_data);
    if (self && oldVoxel && newVoxel) self->onVoxelModified(pos, *oldVoxel, *newVoxel);
}

void FluidSystem::onVoxelModified(const WorldCoord& pos, const Voxel& oldVoxel,
                                  const Voxel& newVoxel) {
    if (!terminal_) return;
    const VoxelCoord c = chunkmath::worldToVoxel(pos, terminal_->voxelSizeM());
    const bool wasFluid = isFluidVoxel(oldVoxel);
    const bool isFluid  = isFluidVoxel(newVoxel);
    if (isFluid && !wasFluid) realizedVoxels_.insert(c);
    else if (!isFluid && wasFluid) realizedVoxels_.erase(c);
}

void FluidSystem::seedChunk(ChunkCoord coord) {
    if (!terminal_) return;
    const Chunk* chunk = terminal_->getChunk(coord);
    if (!chunk) return;
    const int n = terminal_->chunkSizeVoxels();
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const Voxel& v = chunk->at(x, y, z);
                if (!isFluidVoxel(v)) continue;
                const VoxelCoord vc = chunkmath::chunkLocalToVoxel(coord, x, y, z, n);
                realizedVoxels_.insert(vc);
                overlay_.set(vc, tuning::fluid::kSaturationThreshold);
            }
}

void FluidSystem::dropChunk(ChunkCoord coord) {
    if (!terminal_) return;
    const int n = terminal_->chunkSizeVoxels();
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const VoxelCoord vc = chunkmath::chunkLocalToVoxel(coord, x, y, z, n);
                overlay_.clear(vc);
                realizedVoxels_.erase(vc);
                announcedRising_.erase(vc);
            }
}

void FluidSystem::applySources(double dt) {
    if (!terminal_) return;
    for (const auto& src : pm_.fluidSources()) {
        const VoxelCoord c = chunkmath::worldToVoxel(src.pos, terminal_->voxelSizeM());
        if (!resident(c)) continue;
        overlay_.set(c, overlay_.get(c) + src.rate * static_cast<float>(dt));
    }
}

void FluidSystem::topUpReservoirs() {
    namespace tf = tuning::fluid;
    for (const VoxelCoord& c : realizedVoxels_)
        if (overlay_.get(c) < tf::kSaturationThreshold)
            overlay_.set(c, tf::kSaturationThreshold);
}

void FluidSystem::fireFluidEvent(const VoxelCoord& c, float amount, FieldCrossing crossing) {
    FluidEvent ev{};
    ev.position = chunkmath::voxelCenter(c, terminal_->voxelSizeM());
    ev.voxel_x  = c.x;
    ev.voxel_y  = c.y;
    ev.voxel_z  = c.z;
    ev.amount   = amount;
    ev.crossing = crossing;
    // The overlay tracks one generic fluid amount per cell, not a per-source
    // identity; the first registered fluid source names the material the
    // mandatory flow plugin realizes (multiple distinct fluid materials
    // sharing one overlay is out of scope for M14's decoupled-fields design).
    if (!pm_.fluidSources().empty()) {
        const auto& src = pm_.fluidSources().front();
        ev.material_id   = src.fluid_material.c_str();
        ev.palette_index = src.palette_index;
    }
    for (const auto& hook : pm_.fluidEventHooks())
        if (hook.fn) hook.fn(&ev, hook.user_data);
}

void FluidSystem::settle(const VoxelCoord& c, float newAmount, int& eventBudget) {
    namespace tf = tuning::fluid;
    overlay_.set(c, newAmount);

    const bool wasRising = announcedRising_.count(c) != 0;
    if (!wasRising && newAmount >= tf::kSaturationThreshold) {
        if (eventBudget <= 0) { carryCells_.insert(c); return; }
        --eventBudget;
        fireFluidEvent(c, newAmount, FieldCrossing::Rising);
        announcedRising_.insert(c);
        ++eventsLastTick_;
    } else if (wasRising && newAmount < tf::kMinFluidAmount) {
        if (eventBudget <= 0) { carryCells_.insert(c); return; }
        --eventBudget;
        fireFluidEvent(c, newAmount, FieldCrossing::Falling);
        announcedRising_.erase(c);
        ++eventsLastTick_;
    }
}

void FluidSystem::tick(double dt) {
    eventsLastTick_ = 0;
    if (!active()) return;
    namespace tf = tuning::fluid;

    applySources(dt);
    topUpReservoirs();

    // Candidate cells: carried overflow + the active set + its frontier,
    // merged in sorted order (§4 determinism).
    std::set<VoxelCoord, VoxelCoordLess> cand(carryCells_.begin(), carryCells_.end());
    carryCells_.clear();
    for (const VoxelCoord& c : overlay_.activeSorted())   cand.insert(c);
    for (const VoxelCoord& c : overlay_.frontierSorted()) cand.insert(c);
    if (cand.empty()) return;

    std::vector<VoxelCoord> work(cand.begin(), cand.end());  // already sorted
    const int maxCells = engineConfig().fluidMaxCellsPerFrame;
    if (static_cast<int>(work.size()) > maxCells) {
        for (std::size_t i = static_cast<std::size_t>(maxCells);
             i < work.size(); ++i)
            carryCells_.insert(work[i]);
        work.resize(maxCells);
    }

    // Delta accumulation so the pass is order-independent within a step:
    // every move is decided from current-plus-already-decided amounts, then
    // applied all at once, conserving total fluid amount exactly.
    std::unordered_map<VoxelCoord, float, VoxelCoordHash> delta;
    auto cur = [&](const VoxelCoord& c) {
        auto it = delta.find(c);
        return overlay_.get(c) + (it != delta.end() ? it->second : 0.0f);
    };
    auto addDelta = [&](const VoxelCoord& c, float d) { delta[c] += d; };

    // The 6 neighbor directions in a fixed order — -Y first so the constant -Y
    // default below reproduces M14's "drain into -Y, then equalize across the 4
    // XZ neighbors" byte-for-byte (the only downhill direction is -Y and the only
    // perpendicular ones are ±X/±Z, in this -X,+X,-Z,+Z order).
    static constexpr std::array<std::array<int64_t, 3>, 6> kDirs6{{
        {{0, -1, 0}}, {{0, 1, 0}}, {{-1, 0, 0}}, {{1, 0, 0}}, {{0, 0, -1}}, {{0, 0, 1}}}};
    const double vs = terminal_->voxelSizeM();
    auto gravityDot = [&](const VoxelCoord& c, const std::array<int64_t, 3>& d) {
        const glm::dvec3 g = gravity_.gravityAt(chunkmath::voxelCenter(c, vs));
        return static_cast<double>(d[0]) * g.x + static_cast<double>(d[1]) * g.y +
               static_cast<double>(d[2]) * g.z;
    };

    // Phase A: gravity drain. A neighbor is "downhill" when its direction has a
    // positive component along the gravity vector (dot > 0); capacity at the
    // destination is its remaining headroom scaled by its porosity, so 0 blocks
    // entirely and 1 lets the full headroom through. Under constant -Y exactly one
    // neighbor (-Y) qualifies — identical to the M14 single-drain step. Under a
    // radial well several neighbors drain so fluid pools toward the center from
    // any side; under zero-g none do, and Phase B alone equalizes pressure.
    for (const VoxelCoord& c : work) {
        for (const auto& d : kDirs6) {
            if (gravityDot(c, d) <= 0.0) continue;
            const float avail = cur(c);
            if (avail <= 0.0f) break;
            const VoxelCoord down{c.x + d[0], c.y + d[1], c.z + d[2]};
            const float capacity = std::max(0.0f, 1.0f - cur(down)) * porosityAt(down);
            const float move = std::min(avail, capacity);
            if (move > 0.0f) { addDelta(c, -move); addDelta(down, move); }
        }
    }

    // Phase B: lateral equalization (head-driven) across neighbors PERPENDICULAR
    // to gravity (dot == 0). Under constant -Y this is the 4 horizontal neighbors;
    // under zero-g all 6 directions qualify, so fluid equalizes pressure in every
    // direction with no preferred axis. Moves half the difference so two cells
    // converge rather than swap outright in one step — an oscillation guard, not a
    // tunable.
    for (const VoxelCoord& c : work) {
        for (const auto& d : kDirs6) {
            if (gravityDot(c, d) != 0.0) continue;
            const VoxelCoord n{c.x + d[0], c.y + d[1], c.z + d[2]};
            const float curC = cur(c);
            const float curN = cur(n);
            if (curC <= curN) continue;
            const float diff = curC - curN;
            const float capacity = std::max(0.0f, 1.0f - curN) * porosityAt(n);
            const float move = std::min({diff * 0.5f, curC, capacity});
            if (move > 0.0f) { addDelta(c, -move); addDelta(n, move); }
        }
    }

    // Commit + fire events, in sorted order, within the per-frame event budget;
    // overflow carries to next tick via settle()'s carryCells_ insert. The commit
    // set is every cell that received a delta UNION the work set — a downhill
    // cascade can move fluid to a cell beyond the initial frontier (e.g. draining
    // along +Y processes cells in ascending order and hands fluid forward several
    // hops in one pass), and that destination must be committed or the fluid is
    // lost. Under constant -Y every drain/lateral destination is already in the
    // frontier (⊆ work), so this set equals `work` and the pass is byte-identical
    // to M14.
    std::set<VoxelCoord, VoxelCoordLess> touched(work.begin(), work.end());
    for (const auto& kv : delta) touched.insert(kv.first);

    int eventBudget = engineConfig().fluidMaxEventsPerFrame;
    for (const VoxelCoord& c : touched) {
        auto it = delta.find(c);
        const float newAmount = overlay_.get(c) + (it != delta.end() ? it->second : 0.0f);
        settle(c, newAmount, eventBudget);
    }
}

}  // namespace sim

#include "simulation/FluidSystem.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/PluginManager.h"
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
    if (static_cast<int>(work.size()) > tf::kMaxFluidCellsPerFrame) {
        for (std::size_t i = static_cast<std::size_t>(tf::kMaxFluidCellsPerFrame);
             i < work.size(); ++i)
            carryCells_.insert(work[i]);
        work.resize(tf::kMaxFluidCellsPerFrame);
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

    // Phase A: gravity drain. "Down" is -Y (the engine's Y-up convention —
    // see the kGravity demos' `vy -= kGravity*dt`); capacity at the
    // destination is its remaining headroom scaled by its porosity, so 0
    // blocks entirely and 1 lets the full headroom through.
    for (const VoxelCoord& c : work) {
        const float avail = cur(c);
        if (avail <= 0.0f) continue;
        const VoxelCoord down{c.x, c.y - 1, c.z};
        const float capacity = std::max(0.0f, 1.0f - cur(down)) * porosityAt(down);
        const float move = std::min(avail, capacity);
        if (move > 0.0f) { addDelta(c, -move); addDelta(down, move); }
    }

    // Phase B: lateral equalization (head-driven, the 4 horizontal
    // neighbors). Moves half the difference so two cells converge rather
    // than swap outright in one step — an oscillation guard, not a tunable.
    static constexpr std::array<std::pair<int64_t, int64_t>, 4> kLateral{
        {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
    for (const VoxelCoord& c : work) {
        for (const auto& [dx, dz] : kLateral) {
            const VoxelCoord n{c.x + dx, c.y, c.z + dz};
            const float curC = cur(c);
            const float curN = cur(n);
            if (curC <= curN) continue;
            const float diff = curC - curN;
            const float capacity = std::max(0.0f, 1.0f - curN) * porosityAt(n);
            const float move = std::min({diff * 0.5f, curC, capacity});
            if (move > 0.0f) { addDelta(c, -move); addDelta(n, move); }
        }
    }

    // Commit + fire events, in sorted order, within the per-frame event
    // budget; overflow carries to next tick via settle()'s carryCells_ insert.
    int eventBudget = tf::kMaxFluidEventsPerFrame;
    for (const VoxelCoord& c : work) {
        auto it = delta.find(c);
        const float newAmount = overlay_.get(c) + (it != delta.end() ? it->second : 0.0f);
        settle(c, newAmount, eventBudget);
    }
}

}  // namespace sim

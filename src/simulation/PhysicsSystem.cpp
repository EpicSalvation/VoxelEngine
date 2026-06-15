#include "simulation/PhysicsSystem.h"

#include <vector>

#include "core/PluginManager.h"
#include "core/Tuning.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace sim {

using chunkmath::VoxelCoord;

PhysicsSystem::PhysicsSystem(const World& world, PluginManager& pm)
    : pm_(pm), prop_(world) {
    // PropagationSystem rides the same on_voxel_modified path a plugin would: the
    // engine-owned hook forwards every committed edit (local or replicated) from
    // the choke point to incremental aggregation, with no NetworkManager→sim edge.
    pm_.registerEngineVoxelModifiedHook(&PhysicsSystem::onVoxelModifiedThunk, this);
}

PhysicsSystem::~PhysicsSystem() {
    pm_.unregisterEngineVoxelModifiedHook(this);
}

void PhysicsSystem::onVoxelModifiedThunk(WorldCoord pos, const Voxel* oldVoxel,
                                         const Voxel* newVoxel, PlayerId /*source*/,
                                         void* user_data) {
    auto* self = static_cast<PhysicsSystem*>(user_data);
    if (self && oldVoxel && newVoxel)
        self->prop_.onVoxelModified(pos, *oldVoxel, *newVoxel);
}

void PhysicsSystem::fireEvent(const PropagationSystem::Unstable& u) {
    const Layer* composite = prop_.compositeLayer();
    const Layer* terminal  = prop_.terminalLayer();
    if (!composite || !terminal) return;

    StructuralEvent ev{};
    ev.position           = prop_.macroCenter(u.macro);
    ev.voxel_x            = u.macro.x;
    ev.voxel_y            = u.macro.y;
    ev.voxel_z            = u.macro.z;
    ev.layer_name         = composite->name().c_str();  // valid for the call (Layer outlives us)
    ev.voxel_size_m       = prop_.macroVoxelSizeM();
    ev.child_voxel_size_m = terminal->voxelSizeM();
    ev.aggregate_strength = u.aggregate_strength;
    ev.support_potential  = u.support_potential;

    // Fire to every registered structural-response plugin in registration order.
    // PhysicsSystem only reports — the plugin owns the world write (§7).
    for (const auto& hook : pm_.structuralEventHooks())
        if (hook.fn) hook.fn(&ev, hook.user_data);
}

void PhysicsSystem::tick() {
    eventsLastTick_ = 0;
    if (!prop_.active()) return;

    namespace tp = tuning::physics;

    // ── 1. Candidate macros: those carried from the last tick (budget overflow)
    //    plus the dirty set drained from this tick's edits, merged in sorted order
    //    so the whole pass is deterministic (§4).
    std::set<VoxelCoord, VoxelCoordLess> cand(carry_.begin(), carry_.end());
    carry_.clear();
    for (const VoxelCoord& m : prop_.drainDirty()) cand.insert(m);
    if (cand.empty()) return;

    // ── 2. Recompute the affected aggregates, bounded. Aggregates are normally
    //    maintained incrementally on the edit path, so this re-sum is the fallback
    //    that (re)establishes a baseline; macros past the cap carry to next tick so
    //    a huge dirty burst spreads instead of stalling one frame.
    std::vector<VoxelCoord> active;
    active.reserve(cand.size());
    int recomputeBudget = tp::kMaxAggregateRecomputesPerFrame;
    for (const VoxelCoord& m : cand) {  // sorted order
        if (recomputeBudget <= 0) { carry_.insert(m); continue; }
        --recomputeBudget;
        prop_.recomputeAggregate(m);
        active.push_back(m);
    }
    if (active.empty()) return;

    // ── 3. Support flood over the touched macros + their neighbor cascade.
    const std::vector<PropagationSystem::Unstable> unstable =
        prop_.findUnstable(active);

    // ── 4. Fire on_structural_event for each NEWLY-unstable macro within budget.
    //    A macro already announced and still unstable is not re-fired (the response
    //    plugin's edit lands next tick); events past the cap carry to next tick.
    std::set<VoxelCoord, VoxelCoordLess> nowUnstable;
    int eventBudget = tp::kMaxStructuralEventsPerFrame;
    for (const PropagationSystem::Unstable& u : unstable) {  // already sorted
        nowUnstable.insert(u.macro);
        if (firedUnstable_.count(u.macro)) continue;  // already announced
        if (eventBudget <= 0) { carry_.insert(u.macro); continue; }  // overflow → next tick
        --eventBudget;
        fireEvent(u);
        firedUnstable_.insert(u.macro);
        ++eventsLastTick_;
    }

    // Forget any macro we re-evaluated this tick that is no longer unstable — its
    // support was restored, or the response plugin already cleared it — so it can
    // fire again if it later destabilizes a second time.
    for (const VoxelCoord& m : active)
        if (!nowUnstable.count(m)) firedUnstable_.erase(m);
}

}  // namespace sim

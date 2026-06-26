#include "simulation/PhysicsSystem.h"

#include <vector>

#include "core/PluginManager.h"
#include "core/EngineConfig.h"
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

void PhysicsSystem::fireEvent(int level, const PropagationSystem::Unstable& u) {
    const Layer* composite = prop_.compositeLayer(level);
    const Layer* child     = prop_.childLayerAt(level);
    if (!composite || !child) return;

    StructuralEvent ev{};
    ev.position           = prop_.macroCenter(level, u.macro);
    ev.voxel_x            = u.macro.x;
    ev.voxel_y            = u.macro.y;
    ev.voxel_z            = u.macro.z;
    ev.layer_name         = composite->name().c_str();  // valid for the call (Layer outlives us)
    ev.voxel_size_m       = prop_.macroVoxelSizeM(level);
    ev.child_voxel_size_m = child->voxelSizeM();  // this level's immediate child scale
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

    const int levels = prop_.levelCount();
    if (static_cast<int>(carry_.size()) != levels) carry_.resize(levels);
    if (static_cast<int>(firedUnstable_.size()) != levels) firedUnstable_.resize(levels);

    // The recompute / event budgets are shared across the whole upward sweep, so a
    // deep chain reaction (grandchild edit → parent → grandparent → …) spreads
    // across frames instead of stalling one — overflow at any level carries to
    // that level's next tick (§7 "Performance", gap audit G1).
    int recomputeBudget = engineConfig().physicsMaxAggregateRecomputesPerFrame;
    int eventBudget     = engineConfig().physicsMaxStructuralEventsPerFrame;

    // Process levels fine→coarse. recomputeAggregate at level li marks each macro's
    // parent at level li+1 dirty, so draining li+1 *after* li picks those up and
    // the cascade walks the full ancestor chain within one tick (budget allowing).
    for (int li = 0; li < levels; ++li) {
        // ── 1. Candidate macros: those carried from the last tick (budget overflow)
        //    plus the dirty set drained for this level, merged in sorted order so
        //    the whole pass is deterministic (§4).
        std::set<VoxelCoord, VoxelCoordLess> cand(carry_[li].begin(), carry_[li].end());
        carry_[li].clear();
        for (const VoxelCoord& m : prop_.drainDirty(li)) cand.insert(m);
        if (cand.empty()) continue;

        // ── 2. Recompute the affected aggregates, bounded. This also marks each
        //    macro's coarser-level parent dirty (the upward cascade). Macros past
        //    the cap carry to next tick so a huge dirty burst spreads frames.
        std::vector<VoxelCoord> active;
        active.reserve(cand.size());
        for (const VoxelCoord& m : cand) {  // sorted order
            if (recomputeBudget <= 0) { carry_[li].insert(m); continue; }
            --recomputeBudget;
            prop_.recomputeAggregate(li, m);
            active.push_back(m);
        }
        if (active.empty()) continue;

        // ── 3. Support flood over the touched macros + their neighbor cascade at
        //    this level.
        const std::vector<PropagationSystem::Unstable> unstable =
            prop_.findUnstable(li, active);

        // ── 4. Fire on_structural_event for each NEWLY-unstable macro within the
        //    shared budget. A macro already announced and still unstable is not
        //    re-fired (the response plugin's edit lands next tick); events past the
        //    cap carry to next tick.
        std::set<VoxelCoord, VoxelCoordLess> nowUnstable;
        for (const PropagationSystem::Unstable& u : unstable) {  // already sorted
            nowUnstable.insert(u.macro);
            if (firedUnstable_[li].count(u.macro)) continue;  // already announced
            if (eventBudget <= 0) { carry_[li].insert(u.macro); continue; }  // overflow → next tick
            --eventBudget;
            fireEvent(li, u);
            firedUnstable_[li].insert(u.macro);
            ++eventsLastTick_;
        }

        // Forget any macro we re-evaluated this tick that is no longer unstable —
        // its support was restored, or the response plugin already cleared it — so
        // it can fire again if it later destabilizes a second time.
        for (const VoxelCoord& m : active)
            if (!nowUnstable.count(m)) firedUnstable_[li].erase(m);
    }
}

}  // namespace sim

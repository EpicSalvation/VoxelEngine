#pragma once

#include <cstddef>
#include <set>
#include <vector>

#include "simulation/PropagationSystem.h"

class PluginManager;
class World;

// ---------------------------------------------------------------------------
// PhysicsSystem — the DRIVER half of M13 upward damage propagation
// (docs/ARCHITECTURE.md §7). It owns the detection-only PropagationSystem, runs
// one deferred pass per frame within the tuning::physics budgets, and fires
// on_structural_event for each newly-unstable macro. Like PropagationSystem it
// writes no voxels: the registered structural-response plugin owns every world
// edit, applied through the public edit path (ctx.apply_edit). With no such
// plugin loaded the engine still detects and fires, but the world stays
// byte-identical — the "no structural plugin ⇒ no cave-ins" contract (§7).
//
// Wiring (§13 dependency map). PhysicsSystem depends on World (read, via
// PropagationSystem), PropagationSystem, and PluginManager (fire). It does NOT
// touch Renderer / DecompositionWorker / IO and never calls setVoxel. The host
// drives it once per frame at end-of-frame, alongside DecompositionManager::tick:
//
//     decompMgr.tick(...);     // streaming / decomposition
//     // ... player edits applied this frame (through NetworkManager::applyEdit)
//     physics.tick();          // drain dirty → flood → fire structural events
//
// PropagationSystem observes edits at the single edit choke point by riding the
// on_voxel_modified hook: the constructor registers an engine-owned hook that
// forwards each committed edit to PropagationSystem::onVoxelModified. This keeps
// NetworkManager free of any simulation-tier dependency (§13) — the structural
// system sees exactly what a plugin's on_voxel_modified would.
//
// The cascade is a feedback loop, never an in-engine recursion: tick() fires
// events; the response plugin edits via ctx.apply_edit → NetworkManager::applyEdit
// → World::setVoxel → on_voxel_modified → the macro is re-dirtied; the NEXT tick
// re-evaluates and finds the next ring of unstable macros, terminating where the
// structure meets an anchor.
// ---------------------------------------------------------------------------

namespace sim {

class PhysicsSystem {
public:
    // Builds the owned PropagationSystem over world and registers the engine-owned
    // on_voxel_modified hook on pm so edits at the choke point reach detection.
    PhysicsSystem(const World& world, PluginManager& pm);
    ~PhysicsSystem();

    PhysicsSystem(const PhysicsSystem&)            = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    // End-of-frame propagation pass (§7 "Performance"). Drains the dirty-aggregate
    // set (plus any macros carried from last frame), recomputes the affected
    // aggregates (≤ kMaxAggregateRecomputesPerFrame), runs the support flood over
    // the touched macros, and fires on_structural_event for each newly-unstable
    // macro up to kMaxStructuralEventsPerFrame. Overflow — recomputes and events
    // beyond their caps — carries to the next frame so a large chain reaction
    // spreads across frames instead of stalling one. Fires nothing and carries
    // nothing when there is no work or no composite level (PropagationSystem
    // inactive). Never writes a voxel.
    void tick();

    // ── HUD / test introspection ──────────────────────────────────────────────
    // Structural events fired during the most recent tick().
    int eventsFiredLastTick() const { return eventsLastTick_; }
    // Macros waiting to be (re-)evaluated next tick because they overflowed a
    // per-frame budget — the carried-overflow backlog the demo HUD surfaces,
    // summed across every composite level.
    std::size_t carryBacklog() const {
        std::size_t n = 0;
        for (const auto& s : carry_) n += s.size();
        return n;
    }

    PropagationSystem&       propagation()       { return prop_; }
    const PropagationSystem& propagation() const { return prop_; }

private:
    void fireEvent(int level, const PropagationSystem::Unstable& u);

    using MacroSet = std::set<chunkmath::VoxelCoord, VoxelCoordLess>;

    // Engine-owned on_voxel_modified trampoline (registered on PluginManager).
    static void onVoxelModifiedThunk(WorldCoord pos, const Voxel* oldVoxel,
                                     const Voxel* newVoxel, PlayerId source,
                                     void* user_data);

    PluginManager&    pm_;
    PropagationSystem prop_;

    // Per composite level (sized lazily to prop_.levelCount()):
    //   carry_ — macros to (re-)evaluate next tick: those that overflowed the
    //            recompute or event budget this tick. Sorted-coord order so the
    //            carried set — and the fired sequence across frames — is reproducible.
    //   firedUnstable_ — macros already announced as unstable and still unstable:
    //            suppresses re-firing the same macro every frame while the response
    //            plugin works on it.
    // Kept per-level because the same VoxelCoord names different macros at
    // different scales.
    std::vector<MacroSet> carry_;
    std::vector<MacroSet> firedUnstable_;

    int eventsLastTick_ = 0;
};

}  // namespace sim

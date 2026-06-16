#pragma once

#include <cstddef>
#include <cstdint>
#include <set>

#include "simulation/FieldOverlay.h"
#include "simulation/NeighborWalk.h"
#include "world/ChunkCoordMath.h"
#include "world/Voxel.h"  // Voxel, PlayerId (via plugin_api.h)

class PluginManager;
class World;
class Layer;

// ---------------------------------------------------------------------------
// FluidSystem — M14 cellular-automaton fluid flow (docs/ARCHITECTURE.md §17).
// Owns a FieldOverlay of in-flight fluid amount (ambient/absent default 0 — no
// fluid) and advances it each tick with a level/head relaxation gated by each
// destination cell's `porosity` (0 blocks entirely, 1 — and air/empty,
// treated as effective 1.0 — is fully permeable), at terminal-voxel
// granularity bounded to the resident region. Reads World; writes only its
// own overlay, never World::setVoxel (§13). A cell crossing
// tuning::fluid::kSaturationThreshold fires on_fluid_event(Rising) so the
// mandatory `flow` plugin can realize it as a voxel; a realized cell draining
// below tuning::fluid::kMinFluidAmount fires on_fluid_event(Falling) so the
// plugin clears it.
//
// Overlay <-> voxel coherence: FluidSystem rides an engine-owned
// on_voxel_modified hook (the PropagationSystem/PhysicsSystem pattern) to
// track which resident terminal voxels are already-realized fluid (their
// palette_index matches a registered fluid source's material). Each tick,
// before computing flow, every such voxel's overlay cell is topped back up to
// at least kSaturationThreshold — an infinite-reservoir model that needs no
// special case inside the conservation-of-amount delta loop: even if a tick's
// flow drains it, the next tick's top-up restores it.
//
// Wiring (§13 dependency map). Like PhysicsSystem and ThermalSystem, the host
// drives it once per frame at end-of-frame, after that frame's edits applied
// (seedChunk/dropChunk are called separately, on the host's chunk-residency
// transitions):
//
//     decompMgr.tick(...);     // streaming / decomposition
//     physics.tick();          // structural detection (M13)
//     // ... player/network edits applied this frame
//     thermal.tick(dt);
//     fluid.tick(dt);          // top up reservoirs → flow → fire on_fluid_event
//
// FluidSystem touches only World (read) and PluginManager (fire + read
// registered sources); it never depends on Renderer, IO, or
// DecompositionWorker, and NetworkManager never depends on it — FluidSystem
// observes edits by riding an engine-owned on_voxel_modified hook the same
// way PropagationSystem/PhysicsSystem do, so applyEdit stays free of any
// simulation-tier coupling.
// ---------------------------------------------------------------------------

namespace sim {

class FluidSystem {
public:
    explicit FluidSystem(const World& world, PluginManager& pm);
    ~FluidSystem();

    FluidSystem(const FluidSystem&)            = delete;
    FluidSystem& operator=(const FluidSystem&) = delete;

    bool active() const { return terminal_ != nullptr; }

    // Chunk-residency transient-state hooks (no §9 format change — durable
    // fluid is the realized voxels; the overlay is re-derived). A host that
    // streams chunks calls these when a chunk becomes resident / evicts.
    //
    // seedChunk scans the chunk's resident voxels for already-realized fluid
    // (palette_index matching a registered fluid source) and seeds the
    // overlay/realized-set so flow continues out of pre-existing fluid voxels
    // immediately, with no one-tick gap waiting for an edit to re-trigger the
    // on_voxel_modified hook.
    void seedChunk(ChunkCoord coord);
    // dropChunk clears every overlay cell and realized-fluid entry inside the
    // chunk, scoping the overlay back out of a region that just evicted.
    void dropChunk(ChunkCoord coord);

    // End-of-frame budgeted flow pass: injects registered fluid emitters, tops
    // up realized-fluid reservoirs, then relaxes the active set + frontier by
    // gravity drain + lateral equalization gated by destination porosity.
    // Bounded by tuning::fluid::kMaxFluidCellsPerFrame /
    // kMaxFluidEventsPerFrame; overflow of either carries to the next frame.
    void tick(double dt);

    float amountAt(const chunkmath::VoxelCoord& c) const { return overlay_.get(c); }
    float amountAt(const WorldCoord& pos) const;

    std::size_t activeCount()         const { return overlay_.activeCount(); }
    int         eventsFiredLastTick() const { return eventsLastTick_; }
    std::size_t carryBacklog()        const { return carryCells_.size(); }

private:
    static void onVoxelModifiedThunk(WorldCoord pos, const Voxel* oldVoxel,
                                     const Voxel* newVoxel, PlayerId source,
                                     void* user_data);
    void onVoxelModified(const WorldCoord& pos, const Voxel& oldVoxel,
                        const Voxel& newVoxel);

    bool   resident(const chunkmath::VoxelCoord& c) const;
    // Effective flow permeability at c: a resident voxel's own porosity, or
    // 1.0 for empty/air and for a non-resident cell (open space until proven
    // otherwise — the conservative default for an unstreamed region).
    float  porosityAt(const chunkmath::VoxelCoord& c) const;
    // True if a resident voxel's material palette_index matches one of the
    // currently-registered fluid sources (the realized-fluid identity test).
    bool   isFluidVoxel(const Voxel& v) const;

    void   applySources(double dt);
    void   topUpReservoirs();
    // Writes newAmount to the overlay cell and fires on_fluid_event if this
    // crosses kSaturationThreshold (rising) or kMinFluidAmount while
    // previously realized (falling). eventBudget is decremented per fire;
    // overflow is recorded in carryCells_ for next tick's candidate set.
    void   settle(const chunkmath::VoxelCoord& c, float newAmount, int& eventBudget);
    void   fireFluidEvent(const chunkmath::VoxelCoord& c, float amount,
                          FieldCrossing crossing);

    const World&    world_;
    PluginManager&  pm_;
    const Layer*    terminal_ = nullptr;

    FieldOverlay overlay_;

    // Resident terminal voxels currently realized as fluid geometry (kept in
    // sync reactively by the on_voxel_modified hook + seedChunk/dropChunk).
    std::set<chunkmath::VoxelCoord, VoxelCoordLess> realizedVoxels_;
    // Cells/announcement state carried across ticks for budget overflow and
    // rising/falling de-dup (a cell already announced rising is not re-fired
    // every tick while still saturated).
    std::set<chunkmath::VoxelCoord, VoxelCoordLess> carryCells_;
    std::set<chunkmath::VoxelCoord, VoxelCoordLess> announcedRising_;

    int eventsLastTick_ = 0;
};

}  // namespace sim

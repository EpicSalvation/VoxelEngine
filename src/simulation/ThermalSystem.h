#pragma once

#include <cstddef>

#include "simulation/FieldOverlay.h"
#include "world/ChunkCoordMath.h"

class PluginManager;
class World;
class Layer;

// ---------------------------------------------------------------------------
// ThermalSystem — M14 heat diffusion (docs/ARCHITECTURE.md §17). Owns a
// FieldOverlay of temperature (ambient default tuning::thermal::kAmbientTemperature)
// and advances it with an explicit, sub-stepped finite-difference relaxation
// over the active set + 6-connected frontier, at terminal-voxel granularity
// bounded to the resident region. Reads World (thermal_conductivity, chunk
// residency) and fires on_thermal_event through PluginManager; it never calls
// World::setVoxel (the §13 "engine never writes voxels" invariant — what a
// plugin does with a temperature crossing is game policy, not this system's
// concern).
//
// Unlike PhysicsSystem's dirty-aggregate carry, a cell skipped this tick for
// budget reasons needs no explicit carry bookkeeping: it simply remains in the
// overlay's own active set/frontier and is reconsidered automatically next
// tick (tuning::thermal::kMaxThermalCellsPerFrame).
//
// Wiring (§13 dependency map). Like PhysicsSystem, the host drives it once per
// frame at end-of-frame, after that frame's edits have applied:
//
//     decompMgr.tick(...);     // streaming / decomposition
//     physics.tick();          // structural detection (M13)
//     // ... player/network edits applied this frame
//     thermal.tick(dt);        // diffuse → fire on_thermal_event
//
// ThermalSystem touches only World (read) and PluginManager (fire); it never
// depends on Renderer, IO, or DecompositionWorker, and reads conductivity live
// each tick rather than riding on_voxel_modified — unlike FluidSystem, it has
// no incremental aggregate that needs keeping in sync with edits.
// ---------------------------------------------------------------------------

namespace sim {

class ThermalSystem {
public:
    // Discovers the terminal (primary) layer from world to read thermal_conductivity
    // and chunk residency from.
    explicit ThermalSystem(const World& world, PluginManager& pm);
    ~ThermalSystem();

    ThermalSystem(const ThermalSystem&)            = delete;
    ThermalSystem& operator=(const ThermalSystem&) = delete;

    bool active() const { return terminal_ != nullptr; }

    // End-of-frame diffusion pass: injects registered heat emitters, then
    // relaxes the active set + frontier toward their neighbors, sub-stepped to
    // respect tuning::thermal::kStabilityFactor for the most conductive cell
    // touched this tick, bounded by tuning::thermal::kMaxThermalCellsPerFrame.
    // Fires on_thermal_event once per ambient-boundary crossing (rising when a
    // cell leaves ambient, falling when it decays back to it).
    void tick(double dt);

    // Read-only field query (the Engine::temperatureAt forwarder reads this).
    float temperatureAt(const chunkmath::VoxelCoord& c) const { return overlay_.get(c); }
    float temperatureAt(const WorldCoord& pos) const;

    std::size_t activeCount()        const { return overlay_.activeCount(); }
    int         eventsFiredLastTick() const { return eventsLastTick_; }

private:
    bool  resident(const chunkmath::VoxelCoord& c) const;
    float conductivityAt(const chunkmath::VoxelCoord& c) const;
    void  applySources(double dt);
    // Writes newT to the overlay and fires on_thermal_event if this crosses the
    // ambient boundary in either direction.
    void  relax(const chunkmath::VoxelCoord& c, float newT);

    const World&   world_;
    PluginManager&  pm_;
    const Layer*    terminal_ = nullptr;

    FieldOverlay overlay_;
    int eventsLastTick_ = 0;
};

}  // namespace sim

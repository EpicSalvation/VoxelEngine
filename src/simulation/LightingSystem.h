#pragma once

#include <cstddef>

#include "simulation/FieldOverlay.h"
#include "world/ChunkCoordMath.h"

class PluginManager;
class World;
class Layer;

// ---------------------------------------------------------------------------
// LightingSystem — M17 propagated voxel lighting (docs/ARCHITECTURE.md §17).
// Owns a FieldOverlay of combined brightness (ambient/absent default
// tuning::lighting::kAmbientBrightness) and advances it each tick with:
//
//   1. Sky light: voxels with no opaque block above them in the resident
//      region receive full sky light (kMaxBrightness). Sky light does not
//      propagate laterally — it is a column check only.
//
//   2. Block (emitter) light: materials with light_emission > 0 and
//      plugin-registered point sources inject brightness that propagates
//      via BFS through transparent (empty) voxels, attenuating by
//      kAttenuationPerStep per hop.
//
// The renderer samples light levels per vertex/face via brightnessAt();
// the mesher multiplies vertex color by that value at mesh-build time.
//
// Wiring (§13 dependency map). Like ThermalSystem and FluidSystem, the host
// drives it once per frame at end-of-frame, after edits have applied:
//
//     thermal.tick(dt);
//     fluid.tick(dt);
//     lighting.tick(dt);
//
// LightingSystem touches only World (read) and PluginManager (fire + read
// registered sources); it never depends on Renderer, IO, or
// DecompositionWorker.
// ---------------------------------------------------------------------------

namespace sim {

class LightingSystem {
public:
    explicit LightingSystem(const World& world, PluginManager& pm);
    ~LightingSystem();

    LightingSystem(const LightingSystem&)            = delete;
    LightingSystem& operator=(const LightingSystem&) = delete;

    bool active() const { return terminal_ != nullptr; }

    void tick(double dt);

    float brightnessAt(const chunkmath::VoxelCoord& c) const { return overlay_.get(c); }
    float brightnessAt(const WorldCoord& pos) const;

    std::size_t activeCount()        const { return overlay_.activeCount(); }
    int         eventsFiredLastTick() const { return eventsLastTick_; }

    // Chunk-residency hooks: seed scans for emitting voxels; drop clears cells.
    void seedChunk(ChunkCoord coord);
    void dropChunk(ChunkCoord coord);

private:
    bool  resident(const chunkmath::VoxelCoord& c) const;
    bool  isOpaque(const chunkmath::VoxelCoord& c) const;
    float emissionAt(const chunkmath::VoxelCoord& c) const;
    bool  hasSkyAccess(const chunkmath::VoxelCoord& c) const;

    void  applySourcesAndEmitters();
    void  propagate();
    void  relax(const chunkmath::VoxelCoord& c, float newB);

    static void onVoxelModifiedThunk(WorldCoord pos, const Voxel* oldVoxel,
                                     const Voxel* newVoxel, PlayerId source,
                                     void* user_data);
    void onVoxelModified(const WorldCoord& pos);

    const World&    world_;
    PluginManager&  pm_;
    const Layer*    terminal_ = nullptr;

    FieldOverlay overlay_;
    int eventsLastTick_ = 0;

    bool dirty_ = true;
};

}  // namespace sim

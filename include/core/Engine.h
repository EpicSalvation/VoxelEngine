#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "WorldCoord.h"

class PluginManager;
class World;
namespace net   { class NetworkManager; }
namespace audio { class AudioManager;   }
namespace sim   { class FluidSystem; class ThermalSystem; class LightingSystem; }

class Engine {
public:
    Engine();
    ~Engine();

    // Bind the engine's I/O dispatch to a plugin manager and world, and register
    // the built-in .vox import/export handlers (lower priority than any
    // plugin-registered handler for the same extension).
    void init(PluginManager& pm, World& world);

    // Import a .vox file into the named layer at anchor.
    // Prefers a plugin-registered importer for ".vox" if one exists;
    // otherwise falls back to the built-in VoxImporter.
    // Returns true on success; logs warnings on failure.
    bool importVox(const std::string& path,
                   const std::string& layerName,
                   const WorldCoord&  anchor);

    // Export the named layer's region [minCorner, maxCorner) to path.
    // Prefers a plugin-registered exporter for ".vox" if one exists;
    // otherwise falls back to VoxExporter and emits a LOG_WARN when any
    // voxel in the region carries non-default extended properties (i.e. the
    // .vox format would silently drop them).
    // Returns true on success; logs warnings on failure.
    bool exportVox(const std::string& layerName,
                   const WorldCoord&  minCorner,
                   const WorldCoord&  maxCorner,
                   const std::string& path);

    void start();
    void stop();
    void update(double dt);

    bool   getIsRunning()      const { return isRunning; }
    double getDeltaTime()      const { return deltaTime; }
    void   setTargetFrameRate(int fps) { desiredFrameRate = fps; }
    int    getTargetFrameRate() const { return desiredFrameRate; }

    // Attach a NetworkManager to receive per-tick updates. Null (the default)
    // disables networking; existing single-player demos are unaffected.
    void                  setNetworkManager(net::NetworkManager* nm) { nm_ = nm; }
    net::NetworkManager*  networkManager() const { return nm_; }

    // Attach an AudioManager to receive per-tick updates. Null (the default)
    // disables audio; existing demos and tests are unaffected (ARCHITECTURE §16).
    void                   setAudioManager(audio::AudioManager* am) { am_ = am; }
    audio::AudioManager*   audioManager() const { return am_; }

    // Attach the M14 field systems for read-only query access. Like PhysicsSystem,
    // FluidSystem/ThermalSystem are driven by the host's own frame loop (not by
    // Engine::update) — these setters exist purely so temperatureAt/fluidAmountAt
    // can forward to them, the read analog of how the renderer receives camera
    // data directly rather than through the plugin ABI (§13).
    void                  setFluidSystem(sim::FluidSystem* fs) { fluid_ = fs; }
    sim::FluidSystem*     fluidSystem() const { return fluid_; }
    void                  setThermalSystem(sim::ThermalSystem* ts) { thermal_ = ts; }
    sim::ThermalSystem*   thermalSystem() const { return thermal_; }
    void                  setLightingSystem(sim::LightingSystem* ls) { lighting_ = ls; }
    sim::LightingSystem*  lightingSystem() const { return lighting_; }

    // Read-only field query accessors (M14/M17, docs/ARCHITECTURE.md §17). Return
    // the sparse overlays' ambient/absent-cell default when no system is attached.
    // No write path is exposed here — only the engine-owned solver writes its
    // own overlay.
    float temperatureAt(const WorldCoord& pos) const;
    float fluidAmountAt(const WorldCoord& pos) const;
    float lightAt(const WorldCoord& pos) const;

private:
    void gameLoop();

    PluginManager*        pm_      = nullptr;
    World*                world_   = nullptr;
    net::NetworkManager*  nm_      = nullptr;
    audio::AudioManager*  am_      = nullptr;
    sim::FluidSystem*     fluid_   = nullptr;
    sim::ThermalSystem*   thermal_  = nullptr;
    sim::LightingSystem*  lighting_ = nullptr;

    std::atomic<bool>  isRunning      = false;
    std::thread        gameLoopThread;
    int                desiredFrameRate = 60;
    double             fixedTimeStep    = 0.0;
    double             deltaTime        = 0.0;

    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point previousTime;
};

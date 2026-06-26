#include "core/Engine.h"
#include "Logger.h"
#include "PluginManager.h"
#include "audio/AudioManager.h"
#include "io/VoxImporter.h"
#include "io/VoxExporter.h"
#include "io/QbImporter.h"
#include "io/QbExporter.h"
#include "renderer/BgfxRenderer.h"
#include "world/DecompositionManager.h"
#include "world/Layer.h"
#include "world/World.h"
#include "net/NetworkManager.h"
#include "simulation/FluidSystem.h"
#include "simulation/ThermalSystem.h"
#include "simulation/LightingSystem.h"
#include "core/Tuning.h"

#include <iostream>

Engine::Engine() : isRunning(false) {}

Engine::~Engine()
{
    if (gameLoopThread.joinable())
    {
        stop();
        gameLoopThread.join();
    }
}

void Engine::init(PluginManager& pm, World& world)
{
    pm_    = &pm;
    world_ = &world;
    pm.registerBuiltinHandlers();
    pm.registerBuiltinNoise();  // value/fbm/ridged/worley floor (architecture.md §6)
}

bool Engine::importVox(const std::string& path,
                       const std::string& layerName,
                       const WorldCoord&  anchor)
{
    if (!pm_ || !world_) {
        Log::error("Engine", "importVox called before init");
        return false;
    }

    // Prefer a plugin-registered (non-builtin) importer for ".vox".
    for (const auto& reg : pm_->importers()) {
        if (reg.extension == "vox" && !reg.isBuiltin) {
            // Plugin importer present — not yet bridged to the Layer API;
            // fall through to the built-in path.
            break;
        }
    }

    Layer* layer = world_->layer(layerName);
    if (!layer) {
        Log::warn("Engine", ("importVox: layer not found: " + layerName).c_str());
        return false;
    }

    VoxImporter importer;
    return importer.load(path, *layer, anchor, *pm_);
}

bool Engine::exportVox(const std::string& layerName,
                       const WorldCoord&  minCorner,
                       const WorldCoord&  maxCorner,
                       const std::string& path)
{
    if (!pm_ || !world_) {
        Log::error("Engine", "exportVox called before init");
        return false;
    }

    // Prefer a plugin-registered (non-builtin) exporter for ".vox".
    for (const auto& reg : pm_->exporters()) {
        if (reg.extension == "vox" && !reg.isBuiltin) {
            // Plugin exporter present — not yet bridged to the Layer API;
            // fall through to the built-in path.
            break;
        }
    }

    Layer* layer = world_->layer(layerName);
    if (!layer) {
        Log::warn("Engine", ("exportVox: layer not found: " + layerName).c_str());
        return false;
    }

    // Lossy-property check: scan resident voxels for non-default extended
    // properties that .vox cannot represent. Warn once if any are found.
    bool hasExtended = false;
    for (const auto& [coord, chunk] : layer->chunks()) {
        if (hasExtended) break;
        const int n = chunk->size();
        for (int z = 0; z < n && !hasExtended; ++z)
        for (int y = 0; y < n && !hasExtended; ++y)
        for (int x = 0; x < n && !hasExtended; ++x) {
            const auto& m = chunk->at(x, y, z).material;
            if (m.density != 0.0f || m.structural_strength != 0.0f ||
                m.thermal_conductivity != 0.0f || m.porosity != 0.0f ||
                m.hardness != 0.0f) {
                hasExtended = true;
            }
        }
    }
    if (hasExtended) {
        Log::warn("Engine", "extended voxel properties dropped; register an exporter plugin to preserve them");
    }

    VoxExporter exporter;
    return exporter.save(path, *layer, minCorner, maxCorner);
}

bool Engine::importQb(const std::string& path,
                      const std::string& layerName,
                      const WorldCoord&  anchor)
{
    if (!pm_ || !world_) {
        Log::error("Engine", "importQb called before init");
        return false;
    }

    for (const auto& reg : pm_->importers()) {
        if (reg.extension == "qb" && !reg.isBuiltin) {
            break;
        }
    }

    Layer* layer = world_->layer(layerName);
    if (!layer) {
        Log::warn("Engine", ("importQb: layer not found: " + layerName).c_str());
        return false;
    }

    QbImporter importer;
    return importer.load(path, *layer, anchor, *pm_);
}

bool Engine::exportQb(const std::string& layerName,
                      const WorldCoord&  minCorner,
                      const WorldCoord&  maxCorner,
                      const std::string& path)
{
    if (!pm_ || !world_) {
        Log::error("Engine", "exportQb called before init");
        return false;
    }

    for (const auto& reg : pm_->exporters()) {
        if (reg.extension == "qb" && !reg.isBuiltin) {
            break;
        }
    }

    Layer* layer = world_->layer(layerName);
    if (!layer) {
        Log::warn("Engine", ("exportQb: layer not found: " + layerName).c_str());
        return false;
    }

    bool hasExtended = false;
    for (const auto& [coord, chunk] : layer->chunks()) {
        if (hasExtended) break;
        const int n = chunk->size();
        for (int z = 0; z < n && !hasExtended; ++z)
        for (int y = 0; y < n && !hasExtended; ++y)
        for (int x = 0; x < n && !hasExtended; ++x) {
            const auto& m = chunk->at(x, y, z).material;
            if (m.density != 0.0f || m.structural_strength != 0.0f ||
                m.thermal_conductivity != 0.0f || m.porosity != 0.0f ||
                m.hardness != 0.0f) {
                hasExtended = true;
            }
        }
    }
    if (hasExtended) {
        Log::warn("Engine", "extended voxel properties dropped; register an exporter plugin to preserve them");
    }

    QbExporter exporter;
    return exporter.save(path, *layer, minCorner, maxCorner);
}

void Engine::start()
{
    isRunning = true;
    gameLoopThread = std::thread(&Engine::gameLoop, this);
    std::cout << "Engine started." << std::endl;
}

void Engine::stop()
{
    isRunning = false;

    if (gameLoopThread.joinable())
    {
        gameLoopThread.join();
    }

    std::cout << "Engine stopped." << std::endl;
}

void Engine::update(double dt)
{
    deltaTime = dt;
    // Network update runs after world update and before render (ARCHITECTURE §15).
    if (nm_ && nm_->isActive()) {
        nm_->update(dt);
    }
    // Audio update: re-project emitter positions to camera-local float (§16).
    if (am_) {
        am_->update();
    }
}

float Engine::temperatureAt(const WorldCoord& pos) const {
    return thermal_ ? thermal_->temperatureAt(pos) : 0.0f;
}

float Engine::fluidAmountAt(const WorldCoord& pos) const {
    return fluid_ ? fluid_->amountAt(pos) : 0.0f;
}

float Engine::lightAt(const WorldCoord& pos) const {
    return lighting_ ? lighting_->brightnessAt(pos) : tuning::lighting::kAmbientBrightness;
}

void Engine::gameLoop() {
    fixedTimeStep = 1.0 / desiredFrameRate;
    double accumulatedTime = 0.0;

    while (isRunning) {
        auto currentTime = Clock::now();
        deltaTime = std::chrono::duration<double>(currentTime - previousTime).count();
        previousTime = currentTime;

        accumulatedTime += deltaTime;

        if (accumulatedTime >= fixedTimeStep) {
            update(deltaTime);
            accumulatedTime -= fixedTimeStep;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

EngineMetrics Engine::getMetrics() const {
    EngineMetrics m;
    m.frameTimeSec = deltaTime;
    m.drawCalls    = renderer_ ? renderer_->drawCallCount() : 0;
    m.voiceCount   = am_ ? am_->activeVoiceCount() : 0;
    m.decompInFlight = decompMgr_ ? decompMgr_->inFlight() : 0;

    if (world_) {
        for (const auto& lp : world_->layers()) {
            LayerChunkMetrics lm;
            lm.layerName      = lp->name();
            lm.residentChunks = lp->chunks().size();
            if (decompMgr_)
                lm.decomposedMacros = decompMgr_->decomposedCount(lm.layerName);
            m.layers.push_back(std::move(lm));
        }
    }
    return m;
}

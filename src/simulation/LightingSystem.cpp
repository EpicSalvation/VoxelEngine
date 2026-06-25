#include "simulation/LightingSystem.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <vector>

#include "core/PluginManager.h"
#include "core/Tuning.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace sim {

using chunkmath::VoxelCoord;
using chunkmath::VoxelCoordHash;

namespace {
constexpr float kBrightnessSnapEpsilon = 1e-3f;
}

LightingSystem::LightingSystem(const World& world, PluginManager& pm)
    : world_(world), pm_(pm), overlay_(tuning::lighting::kAmbientBrightness) {
    terminal_ = world_.primaryLayer();
    pm_.registerEngineVoxelModifiedHook(&LightingSystem::onVoxelModifiedThunk, this);
}

LightingSystem::~LightingSystem() {
    pm_.unregisterEngineVoxelModifiedHook(this);
}

float LightingSystem::brightnessAt(const WorldCoord& pos) const {
    if (!terminal_) return overlay_.ambient();
    return overlay_.get(chunkmath::worldToVoxel(pos, terminal_->voxelSizeM()));
}

bool LightingSystem::resident(const VoxelCoord& c) const {
    if (!terminal_) return false;
    const WorldCoord center = chunkmath::voxelCenter(c, terminal_->voxelSizeM());
    const ChunkCoord cc = chunkmath::worldToChunk(center, terminal_->voxelSizeM(),
                                                  terminal_->chunkSizeVoxels());
    return terminal_->getChunk(cc) != nullptr;
}

bool LightingSystem::isOpaque(const VoxelCoord& c) const {
    if (!resident(c)) return false;
    const WorldCoord center = chunkmath::voxelCenter(c, terminal_->voxelSizeM());
    return !terminal_->getVoxel(center).isEmpty();
}

float LightingSystem::emissionAt(const VoxelCoord& c) const {
    if (!resident(c)) return 0.0f;
    const WorldCoord center = chunkmath::voxelCenter(c, terminal_->voxelSizeM());
    return terminal_->getVoxel(center).material.light_emission;
}

bool LightingSystem::hasSkyAccess(const VoxelCoord& c) const {
    namespace tl = tuning::lighting;
    // Walk upward from c+1 checking for opaque blocks. If we reach the top of
    // the resident region without hitting one, the cell has sky access.
    // Limit the walk to avoid unbounded scans.
    constexpr int kMaxSkyCheckHeight = 256;
    for (int dy = 1; dy <= kMaxSkyCheckHeight; ++dy) {
        VoxelCoord above{c.x, c.y + dy, c.z};
        if (!resident(above)) return true;
        if (isOpaque(above)) return false;
    }
    return true;
}

void LightingSystem::applySourcesAndEmitters() {
    if (!terminal_) return;
    namespace tl = tuning::lighting;

    // Plugin-registered point light sources.
    for (const auto& src : pm_.lightSources()) {
        const VoxelCoord vc = chunkmath::worldToVoxel(src.pos, terminal_->voxelSizeM());
        if (!resident(vc)) continue;
        float cur = overlay_.get(vc);
        float want = std::min(src.brightness, tl::kMaxBrightness);
        if (want > cur)
            overlay_.set(vc, want);
    }
}

void LightingSystem::propagate() {
    namespace tl = tuning::lighting;

    // BFS propagation from all active cells (emitters + sky-lit) into their
    // empty neighbors. Each hop attenuates by kAttenuationPerStep. We rebuild
    // the light field from scratch each tick to handle removals correctly.

    // Collect all emitter seeds: material emitters + plugin sources + sky-lit cells.
    struct Seed { VoxelCoord pos; float brightness; };
    std::vector<Seed> seeds;

    // Material emitters from the overlay's active set.
    for (const VoxelCoord& c : overlay_.activeSorted()) {
        float e = emissionAt(c);
        if (e > 0.0f)
            seeds.push_back({c, std::min(e, tl::kMaxBrightness)});
    }

    // Plugin point sources.
    if (terminal_) {
        for (const auto& src : pm_.lightSources()) {
            const VoxelCoord vc = chunkmath::worldToVoxel(src.pos, terminal_->voxelSizeM());
            if (resident(vc))
                seeds.push_back({vc, std::min(src.brightness, tl::kMaxBrightness)});
        }
    }

    // Sky-lit cells: scan the active set for sky access.
    for (const VoxelCoord& c : overlay_.activeSorted()) {
        if (!isOpaque(c) && hasSkyAccess(c))
            seeds.push_back({c, tl::kMaxBrightness});
    }

    // BFS from seeds.
    std::unordered_map<VoxelCoord, float, VoxelCoordHash> lightMap;
    lightMap.reserve(seeds.size() * 4);

    struct BfsEntry { VoxelCoord pos; float brightness; };
    std::queue<BfsEntry> queue;

    for (const Seed& s : seeds) {
        if (s.brightness > lightMap[s.pos]) {
            lightMap[s.pos] = s.brightness;
            queue.push({s.pos, s.brightness});
        }
    }

    int cellBudget = tl::kMaxLightingCellsPerFrame;
    while (!queue.empty() && cellBudget > 0) {
        auto [pos, brightness] = queue.front();
        queue.pop();

        if (brightness < lightMap[pos])
            continue;

        --cellBudget;

        float propagated = brightness - tl::kAttenuationPerStep;
        if (propagated <= tl::kAmbientBrightness)
            continue;

        for (const VoxelCoord& n : neighbors6(pos)) {
            if (!resident(n)) continue;
            if (isOpaque(n)) {
                // Opaque voxels receive light on their surface but don't propagate.
                // They get the incoming propagated value if it's higher.
                if (propagated > lightMap[n]) {
                    lightMap[n] = propagated;
                }
                continue;
            }
            if (propagated > lightMap[n]) {
                lightMap[n] = propagated;
                queue.push({n, propagated});
            }
        }
    }

    // Commit to overlay and fire events.
    int eventBudget = tl::kMaxLightingEventsPerFrame;
    for (const auto& [c, b] : lightMap) {
        float clamped = std::max(b, tl::kAmbientBrightness);
        if (eventBudget > 0)
            relax(c, clamped);
        else
            overlay_.set(c, clamped);
        --eventBudget;
    }
}

void LightingSystem::relax(const VoxelCoord& c, float newB) {
    namespace tl = tuning::lighting;
    if (std::fabs(newB - overlay_.ambient()) < kBrightnessSnapEpsilon)
        newB = overlay_.ambient();

    const bool wasActive = overlay_.isActive(c);
    overlay_.set(c, newB);
    const bool isActiveNow = overlay_.isActive(c);
    if (isActiveNow == wasActive) return;

    LightingEvent ev{};
    ev.position   = chunkmath::voxelCenter(c, terminal_->voxelSizeM());
    ev.voxel_x    = c.x;
    ev.voxel_y    = c.y;
    ev.voxel_z    = c.z;
    ev.brightness = overlay_.get(c);
    ev.crossing   = isActiveNow ? FieldCrossing::Rising : FieldCrossing::Falling;

    for (const auto& hook : pm_.lightingEventHooks())
        if (hook.fn) hook.fn(&ev, hook.user_data);
    ++eventsLastTick_;
}

void LightingSystem::tick(double /*dt*/) {
    eventsLastTick_ = 0;
    if (!active()) return;
    if (!dirty_) return;

    // Clear the overlay and rebuild from scratch. This is simpler and more
    // correct than incremental updates for light removal. The per-frame budget
    // caps the work.
    std::vector<VoxelCoord> prev = overlay_.activeSorted();
    for (const VoxelCoord& c : prev)
        overlay_.clear(c);

    // Seed sky light: scan all resident chunks for sky-accessible cells.
    // Also seed material emitters.
    if (terminal_) {
        for (const auto& [cc, chunkPtr] : terminal_->chunks()) {
            if (!chunkPtr) continue;
            const Chunk& chunk = *chunkPtr;
            const int n = chunk.size();
            for (int z = 0; z < n; ++z) {
                for (int y = 0; y < n; ++y) {
                    for (int x = 0; x < n; ++x) {
                        VoxelCoord vc = chunkmath::chunkLocalToVoxel(
                            cc, x, y, z, terminal_->chunkSizeVoxels());
                        const Voxel& v = chunk.at(x, y, z);
                        if (v.material.light_emission > 0.0f) {
                            float e = std::min(v.material.light_emission,
                                               tuning::lighting::kMaxBrightness);
                            overlay_.set(vc, e);
                        }
                        if (!isOpaque(vc) && hasSkyAccess(vc)) {
                            overlay_.set(vc, tuning::lighting::kMaxBrightness);
                        }
                    }
                }
            }
        }
    }

    applySourcesAndEmitters();
    propagate();
    dirty_ = false;
}

void LightingSystem::seedChunk(ChunkCoord /*coord*/) {
    dirty_ = true;
}

void LightingSystem::dropChunk(ChunkCoord coord) {
    if (!terminal_) return;
    const int cs = terminal_->chunkSizeVoxels();
    for (int z = 0; z < cs; ++z)
        for (int y = 0; y < cs; ++y)
            for (int x = 0; x < cs; ++x)
                overlay_.clear(chunkmath::chunkLocalToVoxel(coord, x, y, z, cs));
    dirty_ = true;
}

void LightingSystem::onVoxelModifiedThunk(WorldCoord /*pos*/, const Voxel* /*oldVoxel*/,
                                          const Voxel* /*newVoxel*/, PlayerId /*source*/,
                                          void* user_data) {
    auto* self = static_cast<LightingSystem*>(user_data);
    self->dirty_ = true;
}

void LightingSystem::onVoxelModified(const WorldCoord& /*pos*/) {
    dirty_ = true;
}

}  // namespace sim

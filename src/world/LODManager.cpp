#include "LODManager.h"

#include "core/EngineConfig.h"

LODManager::LODManager(const LayerConfig& config) : config_(config) {}

const LayerDef* LODManager::layer(const std::string& name) const {
    return config_.findLayer(name);
}

int LODManager::viewDistanceChunks(const std::string& layerName) const {
    const LayerDef* l = layer(layerName);
    return l ? l->view_distance_chunks : 0;
}

int LODManager::evictDistanceChunks(const std::string& layerName) const {
    const LayerDef* l = layer(layerName);
    return l ? l->view_distance_chunks + engineConfig().streamingHysteresisChunks : 0;
}

StreamingVolume LODManager::volumeFor(const std::string& layerName) const {
    const LayerDef* l = layer(layerName);
    StreamingVolume v;
    if (!l) {
        v.radiusChunks = -1;  // unknown layer → empty volume (contains nothing)
        return v;
    }
    v.shape                = l->streaming_shape;
    v.radiusChunks         = l->view_distance_chunks;
    v.shellThicknessChunks = l->shell_thickness_chunks;
    // The vertical band is a box-volume convenience set at runtime by demos; pass
    // it through so a box volume reproduces the pre-M16 disc×band footprint.
    v.yMin = yMin_;
    v.yMax = yMax_;
    return v;
}

std::vector<ChunkCoord> LODManager::desiredChunks(ChunkCoord center,
                                                  const std::string& layerName) const {
    return volumeFor(layerName).desired(center);
}

bool LODManager::withinViewDistance(ChunkCoord center, ChunkCoord coord,
                                    const std::string& layerName) const {
    return volumeFor(layerName).contains(center, coord);
}

bool LODManager::shouldEvict(ChunkCoord center, ChunkCoord coord,
                             const std::string& layerName) const {
    // Outside the load volume grown by the hysteresis margin: the margin keeps a
    // chunk that just crossed the load radius resident a little longer so camera
    // jitter at the boundary does not thrash load/evict.
    return !volumeFor(layerName).expandedBy(engineConfig().streamingHysteresisChunks).contains(center, coord);
}

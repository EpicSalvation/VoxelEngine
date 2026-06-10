#include "LODManager.h"

#include <algorithm>
#include <cstdlib>

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
    return l ? l->view_distance_chunks + kHysteresisChunks : 0;
}

std::vector<ChunkCoord> LODManager::desiredChunks(ChunkCoord center,
                                                  const std::string& layerName) const {
    std::vector<ChunkCoord> result;
    const int r = viewDistanceChunks(layerName);
    if (r < 0) return result;

    // Intersect the configured vertical band with center.y ± r so the load cube
    // always tracks the camera in Y (3D worlds) while the band can further restrict
    // it (e.g. a heightmap that only populates Y=0..1).
    const int yLo = std::max(std::min(yMin_, yMax_), center.y - r);
    const int yHi = std::min(std::max(yMin_, yMax_), center.y + r);
    if (yLo > yHi) return result;
    result.reserve(static_cast<size_t>(2 * r + 1) * (2 * r + 1) * (yHi - yLo + 1));

    for (int y = yLo; y <= yHi; ++y)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx)
                result.push_back(ChunkCoord{center.x + dx, y, center.z + dz});

    return result;
}

bool LODManager::withinViewDistance(ChunkCoord center, ChunkCoord coord,
                                    const std::string& layerName) const {
    const int r = viewDistanceChunks(layerName);
    const int yLo = std::max(std::min(yMin_, yMax_), center.y - r);
    const int yHi = std::min(std::max(yMin_, yMax_), center.y + r);
    if (coord.y < yLo || coord.y > yHi) return false;
    return std::abs(coord.x - center.x) <= r && std::abs(coord.z - center.z) <= r;
}

bool LODManager::shouldEvict(ChunkCoord center, ChunkCoord coord,
                             const std::string& layerName) const {
    const int e = evictDistanceChunks(layerName);
    const int yLo = std::max(std::min(yMin_, yMax_), center.y - e);
    const int yHi = std::min(std::max(yMin_, yMax_), center.y + e);
    if (coord.y < yLo || coord.y > yHi) return true;
    return std::abs(coord.x - center.x) > e || std::abs(coord.z - center.z) > e;
}

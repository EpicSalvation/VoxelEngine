#include "World.h"
#include <algorithm>
#include <iostream>

World::World(int width, int height, int depth)
    : width_(width), height_(height), depth_(depth),
      voxels_(static_cast<size_t>(width * height * depth), Voxel::empty())
{}

World::World(const LayerDef& layer)
    : width_(0), height_(0), depth_(0),
      chunked_(true),
      voxelSizeM_(layer.voxel_size_m),
      chunkSizeVoxels_(layer.chunk_size_voxels)
{}

Chunk* World::loadChunk(ChunkCoord coord, LayerGeneratorFn generator, void* user_data) {
    if (!chunked_) return nullptr;

    auto it = chunks_.find(coord);
    if (it != chunks_.end())
        return it->second.get();

    WorldCoord origin = chunkmath::chunkOrigin(coord, voxelSizeM_, chunkSizeVoxels_);
    auto chunk = std::make_unique<Chunk>(coord, chunkSizeVoxels_, origin);
    if (generator)
        generator(origin, chunkSizeVoxels_, chunk->data(), user_data);

    Chunk* raw = chunk.get();
    chunks_.emplace(coord, std::move(chunk));
    return raw;
}

void World::unloadChunk(ChunkCoord coord) {
    chunks_.erase(coord);
}

const Chunk* World::getChunk(ChunkCoord coord) const {
    auto it = chunks_.find(coord);
    return it == chunks_.end() ? nullptr : it->second.get();
}

void World::generateWorld(LayerGeneratorFn generator, void* user_data) {
    if (!generator) return;
    int grid = std::min({width_, height_, depth_});
    std::vector<Voxel> chunk(static_cast<size_t>(grid * grid * grid));
    generator(WorldCoord{}, grid, chunk.data(), user_data);
    for (int z = 0; z < grid; ++z)
        for (int y = 0; y < grid; ++y)
            for (int x = 0; x < grid; ++x)
                setVoxel(x, y, z, chunk[x + grid * (y + grid * z)]);
}

Voxel World::getVoxel(int x, int y, int z) const {
    if (x < 0 || x >= width_ || y < 0 || y >= height_ || z < 0 || z >= depth_) {
        std::cerr << "[World] getVoxel out of bounds: " << x << " " << y << " " << z << "\n";
        return Voxel::empty();
    }
    return voxels_[idx(x, y, z)];
}

void World::setVoxel(int x, int y, int z, const Voxel& voxel) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_ || z < 0 || z >= depth_) {
        std::cerr << "[World] setVoxel out of bounds: " << x << " " << y << " " << z << "\n";
        return;
    }
    voxels_[idx(x, y, z)] = voxel;
}

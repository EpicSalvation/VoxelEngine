#include "World.h"
#include <algorithm>
#include <iostream>

World::World(int width, int height, int depth)
    : width_(width), height_(height), depth_(depth),
      voxels_(static_cast<size_t>(width * height * depth), Voxel::empty())
{}

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

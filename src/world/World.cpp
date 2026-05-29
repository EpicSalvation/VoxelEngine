#include "World.h"
#include <iostream>

World::World(int width, int height, int depth)
    : width_(width), height_(height), depth_(depth),
      voxels_(static_cast<size_t>(width * height * depth), Voxel::empty())
{}

void World::generateWorld() {
    MaterialProperties stone;
    stone.density             = 2700.0f;
    stone.structural_strength = 0.9f;
    stone.thermal_conductivity = 2.0f;
    stone.hardness            = 0.7f;
    stone.palette_index       = 1;

    MaterialProperties grass;
    grass.density             = 1200.0f;
    grass.structural_strength = 0.3f;
    grass.thermal_conductivity = 0.5f;
    grass.hardness            = 0.2f;
    grass.palette_index       = 2;

    int surface_y = height_ / 2;

    for (int z = 0; z < depth_; ++z) {
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                Voxel v;
                if (y < surface_y - 1)
                    v.material = stone;
                else if (y == surface_y - 1)
                    v.material = grass;
                else
                    v = Voxel::empty();
                setVoxel(x, y, z, v);
            }
        }
    }
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

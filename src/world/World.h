#pragma once

#include <vector>
#include "Voxel.h"

// Single-layer voxel grid — the minimal world container for M1/M3.
// Full multi-layer world management (Layer[], chunk streaming, LayerConfig integration)
// is introduced in M3/M6.
class World {
public:
    World(int width, int height, int depth);

    // Fills the world with a basic heightmap using stone and grass materials.
    void generateWorld();

    Voxel getVoxel(int x, int y, int z) const;
    void  setVoxel(int x, int y, int z, const Voxel& voxel);

    int getWidth()  const { return width_; }
    int getHeight() const { return height_; }
    int getDepth()  const { return depth_; }

private:
    int width_, height_, depth_;
    std::vector<Voxel> voxels_;  // flat row-major: index = x + width*(y + height*z)

    int idx(int x, int y, int z) const { return x + width_ * (y + height_ * z); }
};

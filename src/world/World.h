#pragma once

#include <vector>
#include "Voxel.h"

// Single-layer voxel grid — the minimal world container for M1/M3.
// Full multi-layer world management (Layer[], chunk streaming, LayerConfig integration)
// is introduced in M3/M6.
class World {
public:
    World(int width, int height, int depth);

    // Populates the world using the provided layer generator plugin callback.
    // The generator is called for a single cubic chunk (min side length of the world
    // dimensions) at the world origin. Voxels outside the cubic region stay empty.
    void generateWorld(LayerGeneratorFn generator, void* user_data = nullptr);

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

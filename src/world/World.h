#ifndef WORLD_H
#define WORLD_H

#include <vector>
#include "Voxel.h"

class World {
public:
    World();
    ~World();
    void generateWorld(int width, int height, int depth);
    Voxel getVoxel(int x, int y, int z) const;

private:
    int width, height, depth;
    std::vector<std::vector<std::vector<Voxel>>> voxels;
};

#endif // WORLD_H
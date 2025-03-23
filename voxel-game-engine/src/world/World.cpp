#include "World.h"
#include "Voxel.h"
#include <vector>

class World {
public:
    World(int width, int height, int depth);
    void generateWorld();
    Voxel getVoxel(int x, int y, int z);
    
private:
    int width, height, depth;
    std::vector<std::vector<std::vector<Voxel>>> voxels;
};

World::World(int width, int height, int depth) 
    : width(width), height(height), depth(depth) {
    voxels.resize(width, std::vector<std::vector<Voxel>>(height, std::vector<Voxel>(depth)));
}

void World::generateWorld() {
    // Implementation for generating the voxel world
}

Voxel World::getVoxel(int x, int y, int z) {
    if (x >= 0 && x < width && y >= 0 && y < height && z >= 0 && z < depth) {
        return voxels[x][y][z];
    }
    // Return a default Voxel or handle error
    return Voxel(); // Assuming Voxel has a default constructor
}
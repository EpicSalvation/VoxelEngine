#include "Voxel.h"

Voxel::Voxel(int x, int y, int z, VoxelType type)
    : type(type) {}

Voxel::Voxel()
{
    // Constructor for default Voxel (mainly used during world resize and error handling)
    type = VoxelType::UNDEFINED; // Default to undefined and define during world generation
    x = 0;    // Let's not leave these uninitialized
    y = 0;
    z = 0;
}

std::tuple<int, int, int> Voxel::getPosition() const
{
    return std::tuple{x, y, z};
}

VoxelType Voxel::getType() const
{
    return type;
}
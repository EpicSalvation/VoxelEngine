class Voxel {
public:
    Voxel(int x, int y, int z, int type);

    std::tuple<int, int, int> getPosition() const;
    int getType() const;

private:
    int x, y, z; // Position of the voxel
    int type;    // Type of the voxel (e.g., dirt, stone, etc.)
};
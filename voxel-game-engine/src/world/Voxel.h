#ifndef VOXEL_H
#define VOXEL_H
#include <tuple>
#include "../enums/VoxelEnums.h"

class Voxel
{
public:
    Voxel(int x, int y, int z, VoxelType type);
    Voxel();

    // Factory method to create a voxel of a specific type and return a pointer to it
    static Voxel *createVoxel(int x, int y, int z, VoxelType type = VoxelType::UNDEFINED)
    {
        switch (type)
        {
        case VoxelType::UNDEFINED:
            return new Voxel(x, y, z, VoxelType::UNDEFINED);
        case VoxelType::WIREFRAME:
            return new WireframeVoxel(x, y, z);
        case VoxelType::COLOR:
            return new ColorVoxel(x, y, z);
        case VoxelType::MULTICOLOR:
            return new MulticolorVoxel(x, y, z);
        case VoxelType::TEXTURED:
            return new TexturedVoxel(x, y, z);
        case VoxelType::TEXTURED_INERT:
            return new TexturedInertVoxel(x, y, z);
        case VoxelType::CLEAR_INERT:
            return new ClearInertVoxel(x, y, z);
        default:
            return nullptr; // Handle undefined voxel type
        }
    }

    std::tuple<int, int, int> getPosition() const;
    VoxelType getType() const;

private:
    int x, y, z;    // Position of the voxel
    VoxelType type; // Type of the voxel (e.g., dirt, stone, etc.)
};

// Create derived classes for different voxel types
// UNDEFINED voxels will just use the base class

// WIREFRAME VOXELS
class WireframeVoxel : public Voxel
{
public:
    WireframeVoxel(int x, int y, int z) : Voxel(x, y, z, VoxelType::WIREFRAME) {}

private:
    // Wireframe-specific properties
    std::tuple<int, int, int> wireframeColor = {0, 0, 0}; // Defautl to black wireframe
    int wireframeThickness = 1;                           // Default thickness
};

// COLOR VOXELS
class ColorVoxel : public Voxel
{
public:
    ColorVoxel(int x, int y, int z) : Voxel(x, y, z, VoxelType::COLOR) {}

private:
    // Color-specific properties
    std::tuple<int, int, int> color = {255, 255, 255}; // Default to white color
    bool showWireframe = false;                        // Default to no wireframe
    int transparency = 0;                              // 0-100 scale; default to fully opaque
    bool isSolid = true;                               // Default to solid
    bool isCollidable = true;                          // Default to collidable
    bool isVisible = true;                             // Default to visible

    // Light properties (if applicable)
    // bool isLightSource = false; // Default to not a light source
    // int lightLevel = 0; // Default light level
    // int lightRange = 0; // Default light range
    // int lightColor = 0; // Default light color
    // int lightIntensity = 0; // Default light intensity
    // int lightFalloff = 0; // Default light falloff
};

// MULTICOLOR VOXELS
class MulticolorVoxel : public Voxel
{
public:
    MulticolorVoxel(int x, int y, int z) : Voxel(x, y, z, VoxelType::MULTICOLOR) {}
    MulticolorVoxel(int x, int y, int z,
                    std::tuple<int, int, int> topColor,
                    std::tuple<int, int, int> bottomColor,
                    std::tuple<int, int, int> frontColor,
                    std::tuple<int, int, int> backColor,
                    std::tuple<int, int, int> leftColor,
                    std::tuple<int, int, int> rightColor)
        : Voxel(x, y, z, VoxelType::MULTICOLOR),
          topColor(topColor), bottomColor(bottomColor), frontColor(frontColor),
          backColor(backColor), leftColor(leftColor), rightColor(rightColor) {}
    void setTopColor(std::tuple<int, int, int> color) { topColor = color; }
    void setBottomColor(std::tuple<int, int, int> color) { bottomColor = color; }
    void setFrontColor(std::tuple<int, int, int> color) { frontColor = color; }
    void setBackColor(std::tuple<int, int, int> color) { backColor = color; }
    void setLeftColor(std::tuple<int, int, int> color) { leftColor = color; }
    void setRightColor(std::tuple<int, int, int> color) { rightColor = color; }
    std::tuple<int, int, int> getTopColor() const { return topColor; }
    std::tuple<int, int, int> getBottomColor() const { return bottomColor; }
    std::tuple<int, int, int> getFrontColor() const { return frontColor; }
    std::tuple<int, int, int> getBackColor() const { return backColor; }
    std::tuple<int, int, int> getLeftColor() const { return leftColor; }
    std::tuple<int, int, int> getRightColor() const { return rightColor; }
    void setAllSidesToSingleColor(std::tuple<int, int, int> color)
    {
        topColor = color;
        bottomColor = color;
        frontColor = color;
        backColor = color;
        leftColor = color;
        rightColor = color;
    }

private:
    // Multicolor-specific properties
    // Each face can have a different color.  Faces are determined by their orientation:
    // top faces up (+Z), bottom faces down (-Z), front faces forward (-Y), back faces backward (+Y), left faces left (-X), right faces right (+X)
    // The colors are stored as tuples of RGB values (0-255)
    std::tuple<int, int, int> topColor = {255, 0, 0};     // Default to red
    std::tuple<int, int, int> bottomColor = {0, 255, 0};  // Default to green
    std::tuple<int, int, int> frontColor = {0, 0, 255};   // Default to blue
    std::tuple<int, int, int> backColor = {255, 255, 0};  // Default to yellow
    std::tuple<int, int, int> leftColor = {255, 0, 255};  // Default to magenta
    std::tuple<int, int, int> rightColor = {0, 255, 255}; // Default to cyan
};

// TEXTURED VOXELS
class TexturedVoxel : public Voxel
{
public:
    TexturedVoxel(int x, int y, int z) : Voxel(x, y, z, VoxelType::TEXTURED) {}
};

// TEXTURED_INERT VOXELS
class TexturedInertVoxel : public Voxel
{
public:
    TexturedInertVoxel(int x, int y, int z) : Voxel(x, y, z, VoxelType::TEXTURED_INERT) {}
};

// CLEAR_INERT VOXELS
class ClearInertVoxel : public Voxel
{
public:
    ClearInertVoxel(int x, int y, int z) : Voxel(x, y, z, VoxelType::CLEAR_INERT) {}
};

#endif // VOXEL_H
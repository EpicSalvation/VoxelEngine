#ifndef BGFX_RENDERER_H
#define BGFX_RENDERER_H

#include "Renderer.h"
#include "../world/World.h"
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <vector>

struct VoxelVertex
{
    float x, y, z;
    uint32_t abgr;
    static bgfx::VertexLayout layout;
    static void initLayout();
};

class BgfxRenderer : public Renderer
{
public:
    BgfxRenderer();
    ~BgfxRenderer() override;

    void initialize() override;
    void render() override;
    void drawVoxel(int x, int y, int z) override;
    void setViewport(int width, int height) override;
    void setCameraPosition(float x, float y, float z) override;
    void setCameraRotation(float pitch, float yaw, float roll) override;
    void cleanup() override;
    void shutdown() override;

    // Placeholder for future world rendering
    void renderWorld(const World &world);

private:
    std::vector<bx::Vec3> voxelPositions;
    bgfx::ProgramHandle program;
    bgfx::VertexBufferHandle vbo;
    bgfx::IndexBufferHandle ibo;
    bx::Vec3 cameraPos;
    bx::Vec3 cameraRot;
    uint32_t viewWidth;
    uint32_t viewHeight;
    bool initialized;
};

#endif // BGFX_RENDERER_H

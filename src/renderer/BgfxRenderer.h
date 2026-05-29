#pragma once

#include "Renderer.h"
#include "../world/World.h"
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <vector>

struct VoxelVertex {
    float    x, y, z;
    uint32_t abgr;
    static bgfx::VertexLayout layout;
    static void initLayout();
};

class BgfxRenderer : public Renderer {
public:
    BgfxRenderer();
    ~BgfxRenderer() override;

    void initialize() override;
    void render() override;
    void drawVoxel(const WorldCoord& position) override;
    void setViewport(int width, int height) override;
    void setCameraPosition(const WorldCoord& pos) override;
    void setCameraRotation(float pitch, float yaw, float roll) override;
    void cleanup() override;
    void shutdown() override;

    void renderWorld(const World& world);

private:
    std::vector<WorldCoord> voxelPositions;
    bgfx::ProgramHandle     program;
    bgfx::VertexBufferHandle vbo;
    bgfx::IndexBufferHandle  ibo;
    WorldCoord cameraPos;
    bx::Vec3   cameraRot;
    uint32_t   viewWidth;
    uint32_t   viewHeight;
    bool       initialized;
};

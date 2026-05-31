#pragma once

#include "Renderer.h"
#include "../world/World.h"
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <vector>

class ChunkMesh;

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

    void initialize(const platform::NativeWindowHandles& handles,
                    uint32_t width, uint32_t height) override;
    void render() override;
    void drawVoxel(const WorldCoord& position, uint32_t abgr = 0xffffffff) override;
    void setViewport(int width, int height) override;
    void setCameraPosition(const WorldCoord& pos) override;
    void setCameraRotation(float pitch, float yaw, float roll) override;
    void cleanup() override;
    void shutdown() override;

    // Submit all non-empty voxels in the world using palette-mapped colors.
    void renderWorld(const World& world);

    // Submit a pre-built chunk mesh, placed via a floating-origin model transform
    // of the chunk's world-space origin. Reuses the voxel shader program.
    void renderChunk(const ChunkMesh& mesh, const WorldCoord& chunkOrigin);

private:
    struct PendingVoxel {
        WorldCoord pos;
        uint32_t   abgr;
    };

    struct PendingChunk {
        WorldCoord               origin;
        bgfx::VertexBufferHandle vbh;
        bgfx::IndexBufferHandle  opaqueIbh;
        bgfx::IndexBufferHandle  translucentIbh;
    };

    std::vector<PendingVoxel> pendingVoxels;
    std::vector<PendingChunk> pendingChunks;
    bgfx::ProgramHandle       program;
    bgfx::IndexBufferHandle   ibo;       // shared cube indices; vertices are per-voxel transient
    WorldCoord                cameraPos;
    bx::Vec3                  cameraRot; // {pitch, yaw, roll} in radians
    uint32_t                  viewWidth;
    uint32_t                  viewHeight;
    bool                      initialized;
};

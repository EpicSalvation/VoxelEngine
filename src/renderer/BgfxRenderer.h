#pragma once

#include "Renderer.h"
#include "../world/World.h"
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <string>
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
    // of the chunk's world-space origin. Reuses the voxel shader program. The mesh
    // is in chunk-local units (1 voxel == 1 unit); voxelSizeM scales it to the
    // layer's world scale, so composite/immutable layers render at their own size.
    void renderChunk(const ChunkMesh& mesh, const WorldCoord& chunkOrigin,
                     double voxelSizeM = 1.0);

    // Submit a wireframe box centered at a world-space position, drawn as lines
    // over the scene (depth-tested, no depth write) — used to outline the voxel
    // the player is targeting. size is the cube's full edge length in meters.
    //
    // progress in [0, 1] ramps the outline from abgr toward red as removal work
    // accrues against the target, giving the player a visible "about to break"
    // cue that lasts longer for harder materials. A negative value (the default)
    // leaves the outline at abgr — the plain targeting highlight with no removal
    // in progress.
    void drawVoxelHighlight(const WorldCoord& center, float size,
                            uint32_t abgr = 0xff00ffff, float progress = -1.0f);

    // Set the far clip plane distance in metres (default: 1000 m; increase for
    // multi-layer worlds where the coarsest layer extends many kilometres).
    void setFarClip(float metres) { farClip = metres; }

    // Enable/disable a centered crosshair drawn via bgfx debug text.
    void setCrosshair(bool enabled) { crosshair = enabled; }

    // Replace the top-left HUD overlay, one string per line, drawn via bgfx debug
    // text. Pass an empty vector to clear it. Persists across frames until changed.
    void setHudText(std::vector<std::string> lines) { hudLines = std::move(lines); }

private:
    struct PendingVoxel {
        WorldCoord pos;
        uint32_t   abgr;
    };

    struct PendingChunk {
        WorldCoord               origin;
        double                   voxelSizeM;
        bgfx::VertexBufferHandle vbh;
        bgfx::IndexBufferHandle  opaqueIbh;
        bgfx::IndexBufferHandle  translucentIbh;
    };

    struct PendingHighlight {
        WorldCoord center;
        float      size;
        uint32_t   abgr;
        float      progress;  // [0,1] removal ramp; <0 means plain outline
    };

    std::vector<PendingVoxel>     pendingVoxels;
    std::vector<PendingChunk>     pendingChunks;
    std::vector<PendingHighlight> pendingHighlights;
    bgfx::ProgramHandle       program;
    bgfx::IndexBufferHandle   ibo;       // shared cube indices; vertices are per-voxel transient
    bgfx::IndexBufferHandle   lineIbo;   // shared cube edge indices (12 edges) for highlights
    std::vector<std::string>  hudLines;  // top-left debug-text overlay
    WorldCoord                cameraPos;
    bx::Vec3                  cameraRot; // {pitch, yaw, roll} in radians
    uint32_t                  viewWidth;
    uint32_t                  viewHeight;
    float                     farClip;
    bool                      crosshair;
    bool                      initialized;
};

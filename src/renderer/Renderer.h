#pragma once

#include "WorldCoord.h"

// Abstract renderer interface.
// Camera position uses WorldCoord so the renderer can implement floating-origin
// GPU submission. See docs/ARCHITECTURE.md §9.
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void initialize() = 0;
    virtual void render() = 0;

    // Submit a voxel at world-space position for rendering this frame.
    // The renderer converts to camera-local float internally via toLocalFloat().
    virtual void drawVoxel(const WorldCoord& position) = 0;

    virtual void setViewport(int width, int height) = 0;
    virtual void setCameraPosition(const WorldCoord& pos) = 0;
    virtual void setCameraRotation(float pitch, float yaw, float roll) = 0;
    virtual void cleanup() = 0;
    virtual void shutdown() = 0;
};

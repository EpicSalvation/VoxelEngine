#pragma once

#include "WorldCoord.h"
#include "platform/NativeWindowHandles.h"

#include <cstdint>

// Abstract renderer interface.
// Camera position uses WorldCoord so the renderer can implement floating-origin
// GPU submission. See docs/ARCHITECTURE.md §9.
class Renderer {
public:
    virtual ~Renderer() = default;

    // Initialize the graphics device against an existing native window surface.
    // width/height are the initial framebuffer size in pixels.
    virtual void initialize(const platform::NativeWindowHandles& handles,
                            uint32_t width, uint32_t height) = 0;
    virtual void render() = 0;

    // Submit a voxel at world-space position for rendering this frame.
    // abgr is a packed ABGR color (0xAABBGGRR); defaults to white.
    // The renderer converts position to camera-local float internally via toLocalFloat().
    virtual void drawVoxel(const WorldCoord& position, uint32_t abgr = 0xffffffff) = 0;

    virtual void setViewport(int width, int height) = 0;
    virtual void setCameraPosition(const WorldCoord& pos) = 0;
    virtual void setCameraRotation(float pitch, float yaw, float roll) = 0;

    // Set the world-space "up" direction the camera's pitch/yaw/roll are
    // interpreted against — e.g. the local -gravity up on an arbitrary surface,
    // so a player standing on the +X face of a body sees a level horizon (M17,
    // surface-normal camera orientation; docs/ARCHITECTURE.md §9/§18). The vector
    // need not be normalized. The default (0,1,0) reproduces the historical Y-up
    // basis byte-for-byte; an implementation that does not support reorientation
    // may leave this a no-op (and stay Y-up).
    virtual void setCameraUp(const glm::vec3& worldUp) { (void)worldUp; }

    virtual void cleanup() = 0;
    virtual void shutdown() = 0;
};

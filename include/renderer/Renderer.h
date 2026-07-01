#pragma once

#include "WorldCoord.h"
#include "renderer/Fog.h"
#include "renderer/Sky.h"
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

    // Set the distance-obscurance (atmospheric fog) parameters applied per pixel
    // by the chunk shader — the depth cue that lets streamed geometry emerge out
    // of murk as it refines, hiding the coarse→fine LOD pop and the view-distance
    // chunk-load boundary (M17; docs/ARCHITECTURE.md §9/§18). This is a POLICY the
    // game drives each frame (typically from a content plugin), not a baked engine
    // force — see FogParams. The default (density 0) disables fog so every existing
    // scene renders byte-identically; an implementation may leave this a no-op.
    virtual void setFog(const FogParams& fog) { (void)fog; }

    // Set the background (clear) color the framebuffer is cleared to each frame.
    // Distance fog should fade geometry toward this same color so far geometry
    // dissolves seamlessly into the background instead of leaving a halo at the
    // far plane — i.e. a game pairs setClearColor(c) with a fog color of c. The
    // default (0x303030 dark gray) reproduces the historical clear byte-for-byte;
    // an implementation may leave this a no-op.
    virtual void setClearColor(const glm::vec3& rgb) { (void)rgb; }

    // Set the procedural-sky (background) parameters — a view-direction gradient
    // the renderer paints behind the scene, distinct from the flat clear color
    // above (M17; docs/architecture.md §9). This is groundwork for the M19
    // "No Man's Voxel" demo, where flying out of a world's bounds needs a
    // convincing sky/space backdrop rather than a flat fill. Like fog, this is a
    // POLICY the game drives (typically from the procedural-sky content plugin) —
    // see SkyParams. The default (enabled == false) draws no sky so every existing
    // scene renders byte-identically; an implementation may leave this a no-op.
    virtual void setSky(const SkyParams& sky) { (void)sky; }

    virtual void cleanup() = 0;
    virtual void shutdown() = 0;
};

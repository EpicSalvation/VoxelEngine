#ifndef RENDERER_H
#define RENDERER_H

#ifndef BX_CONFIG_DEBUG
#define BX_CONFIG_DEBUG 0
#endif // BX_CONFIG_DEBUG


class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void initialize() = 0;
    virtual void render() = 0;
    virtual void drawVoxel(int x, int y, int z) = 0;
    virtual void setViewport(int width, int height) = 0;
    virtual void setCameraPosition(float x, float y, float z) = 0;
    virtual void setCameraRotation(float pitch, float yaw, float roll) = 0;
    virtual void cleanup() = 0;
    virtual void shutdown() = 0;
};

#endif // RENDERER_H
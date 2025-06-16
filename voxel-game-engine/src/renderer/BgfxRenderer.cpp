#include "BgfxRenderer.h"
#include <bgfx/platform.h>
#include <iostream>

bgfx::VertexLayout VoxelVertex::layout;

void VoxelVertex::initLayout()
{
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
}

static VoxelVertex cubeVertices[] = {
    {-0.5f, -0.5f,  0.5f, 0xffffffff},
    { 0.5f, -0.5f,  0.5f, 0xffffffff},
    { 0.5f,  0.5f,  0.5f, 0xffffffff},
    {-0.5f,  0.5f,  0.5f, 0xffffffff},
    {-0.5f, -0.5f, -0.5f, 0xffffffff},
    { 0.5f, -0.5f, -0.5f, 0xffffffff},
    { 0.5f,  0.5f, -0.5f, 0xffffffff},
    {-0.5f,  0.5f, -0.5f, 0xffffffff},
};

static const uint16_t cubeIndices[] = {
    0,1,2, 0,2,3,
    4,5,6, 4,6,7,
    0,1,5, 0,5,4,
    2,3,7, 2,7,6,
    0,3,7, 0,7,4,
    1,2,6, 1,6,5
};

BgfxRenderer::BgfxRenderer()
    : program(BGFX_INVALID_HANDLE),
      vbo(BGFX_INVALID_HANDLE),
      ibo(BGFX_INVALID_HANDLE),
      cameraPos{0.0f, 0.0f, 0.0f},
      cameraRot{0.0f, 0.0f, 0.0f},
      viewWidth(800),
      viewHeight(600),
      initialized(false)
{
}

BgfxRenderer::~BgfxRenderer()
{
    shutdown();
}

void BgfxRenderer::initialize()
{
    bgfx::Init init;
    init.type = bgfx::RendererType::Count;
    init.resolution.width = viewWidth;
    init.resolution.height = viewHeight;
    init.resolution.reset = BGFX_RESET_VSYNC;

    if (!bgfx::init(init))
    {
        std::cerr << "Failed to initialize bgfx" << std::endl;
        return;
    }

    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, viewWidth, viewHeight);

    VoxelVertex::initLayout();
    vbo = bgfx::createVertexBuffer(bgfx::makeRef(cubeVertices, sizeof(cubeVertices)), VoxelVertex::layout);
    ibo = bgfx::createIndexBuffer(bgfx::makeRef(cubeIndices, sizeof(cubeIndices)));

    initialized = true;
}

void BgfxRenderer::render()
{
    if (!initialized)
        return;

    bgfx::touch(0);

    for (const auto &pos : voxelPositions)
    {
        float mtx[16];
        bx::mtxTranslate(mtx, pos.x, pos.y, pos.z);
        bgfx::setTransform(mtx);
        bgfx::setVertexBuffer(0, vbo);
        bgfx::setIndexBuffer(ibo);
        if (bgfx::isValid(program))
        {
            bgfx::submit(0, program);
        }
    }

    bgfx::frame();
    voxelPositions.clear();
}

void BgfxRenderer::drawVoxel(int x, int y, int z)
{
    voxelPositions.emplace_back(float(x), float(y), float(z));
}

void BgfxRenderer::setViewport(int width, int height)
{
    viewWidth = static_cast<uint32_t>(width);
    viewHeight = static_cast<uint32_t>(height);
    bgfx::reset(viewWidth, viewHeight, BGFX_RESET_VSYNC);
    bgfx::setViewRect(0, 0, 0, viewWidth, viewHeight);
}

void BgfxRenderer::setCameraPosition(float x, float y, float z)
{
    cameraPos = {x, y, z};
}

void BgfxRenderer::setCameraRotation(float pitch, float yaw, float roll)
{
    cameraRot = {pitch, yaw, roll};
}

void BgfxRenderer::cleanup()
{
    if (bgfx::isValid(vbo))
    {
        bgfx::destroy(vbo);
        vbo = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(ibo))
    {
        bgfx::destroy(ibo);
        ibo = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(program))
    {
        bgfx::destroy(program);
        program = BGFX_INVALID_HANDLE;
    }
}

void BgfxRenderer::shutdown()
{
    if (!initialized)
        return;

    cleanup();
    bgfx::shutdown();
    initialized = false;
}

void BgfxRenderer::renderWorld(const World & /*world*/)
{
    // Placeholder implementation
}


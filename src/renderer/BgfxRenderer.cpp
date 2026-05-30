#include "BgfxRenderer.h"
#include <bgfx/platform.h>
#include <iostream>

// Restrict the embedded-shader backend matrix to the profiles actually compiled
// by the build (see CMakeLists "Shaders"). Defining these before
// <bgfx/embedded_shader.h> stops BGFX_EMBEDDED_SHADER from referencing bytecode
// arrays that do not exist:
//   - WGSL: never generated (no WebGPU target).
//   - DXIL (Direct3D12): not generated (shaderc/--werror issue; follow-up).
//   - DXBC (Direct3D11): generated on Windows only.
#define BGFX_PLATFORM_SUPPORTS_WGSL 0
#define BGFX_PLATFORM_SUPPORTS_DXIL 0
#if defined(__linux__)
#  define BGFX_PLATFORM_SUPPORTS_DXBC 0
#endif
#include <bgfx/embedded_shader.h>

// Shader bytecode generated from shaders/*.sc by the opt-in VOXEL_BUILD_SHADERS
// build (see CMakeLists "Shaders"); the headers are committed under
// shaders/generated/ so normal builds and CI need no shader toolchain.
#include <generated/spirv/vs_voxel.sc.bin.h>
#include <generated/glsl/vs_voxel.sc.bin.h>
#include <generated/essl/vs_voxel.sc.bin.h>
#include <generated/spirv/fs_voxel.sc.bin.h>
#include <generated/glsl/fs_voxel.sc.bin.h>
#include <generated/essl/fs_voxel.sc.bin.h>
#if defined(_WIN32)
#  include <generated/dxbc/vs_voxel.sc.bin.h>
#  include <generated/dxbc/fs_voxel.sc.bin.h>
#elif defined(__APPLE__)
#  include <generated/metal/vs_voxel.sc.bin.h>
#  include <generated/metal/fs_voxel.sc.bin.h>
#endif

static const bgfx::EmbeddedShader s_embeddedShaders[] = {
    BGFX_EMBEDDED_SHADER(vs_voxel),
    BGFX_EMBEDDED_SHADER(fs_voxel),
    BGFX_EMBEDDED_SHADER_END()
};

bgfx::VertexLayout VoxelVertex::layout;

void VoxelVertex::initLayout() {
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)
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
    1,2,6, 1,6,5,
};

BgfxRenderer::BgfxRenderer()
    : program(BGFX_INVALID_HANDLE),
      vbo(BGFX_INVALID_HANDLE),
      ibo(BGFX_INVALID_HANDLE),
      cameraRot{0.0f, 0.0f, 0.0f},
      viewWidth(800),
      viewHeight(600),
      initialized(false)
{}

BgfxRenderer::~BgfxRenderer() {
    shutdown();
}

void BgfxRenderer::initialize(const platform::NativeWindowHandles& handles,
                              uint32_t width, uint32_t height) {
    viewWidth  = width;
    viewHeight = height;

    // Render on the calling thread (single-threaded mode). Calling renderFrame()
    // before init avoids bgfx spawning its own render thread — simpler and
    // required on platforms (notably macOS) where window/device calls must stay
    // on the main thread.
    bgfx::renderFrame();

    bgfx::Init init;
    init.type              = bgfx::RendererType::Count;  // auto-select per platform
    init.resolution.width  = viewWidth;
    init.resolution.height = viewHeight;
    init.resolution.reset  = BGFX_RESET_VSYNC;

    init.platformData.nwh = handles.window;
    init.platformData.ndt = handles.display;
    if (handles.wayland)
        init.platformData.type = bgfx::NativeWindowHandleType::Wayland;

    if (!bgfx::init(init)) {
        std::cerr << "[BgfxRenderer] Failed to initialize bgfx\n";
        return;
    }

    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, static_cast<uint16_t>(viewWidth), static_cast<uint16_t>(viewHeight));

    VoxelVertex::initLayout();
    vbo = bgfx::createVertexBuffer(bgfx::makeRef(cubeVertices, sizeof(cubeVertices)), VoxelVertex::layout);
    ibo = bgfx::createIndexBuffer(bgfx::makeRef(cubeIndices, sizeof(cubeIndices)));

    // Select the bytecode matching the backend bgfx actually chose at init.
    const bgfx::RendererType::Enum rendererType = bgfx::getRendererType();
    bgfx::ShaderHandle vsh = bgfx::createEmbeddedShader(s_embeddedShaders, rendererType, "vs_voxel");
    bgfx::ShaderHandle fsh = bgfx::createEmbeddedShader(s_embeddedShaders, rendererType, "fs_voxel");
    program = bgfx::createProgram(vsh, fsh, true /* destroy shaders with program */);
    if (!bgfx::isValid(program))
        std::cerr << "[BgfxRenderer] Failed to create shader program\n";

    initialized = true;
}

void BgfxRenderer::render() {
    if (!initialized) return;

    bgfx::touch(0);

    for (const auto& worldPos : voxelPositions) {
        // Floating origin: convert world-space position to camera-local float.
        // This keeps vertex values small in magnitude, preserving float32 precision
        // even at large world scales. See docs/ARCHITECTURE.md §9.
        glm::vec3 localPos = worldPos.toLocalFloat(cameraPos);
        float mtx[16];
        bx::mtxTranslate(mtx, localPos.x, localPos.y, localPos.z);
        bgfx::setTransform(mtx);
        bgfx::setVertexBuffer(0, vbo);
        bgfx::setIndexBuffer(ibo);
        if (bgfx::isValid(program))
            bgfx::submit(0, program);
    }

    bgfx::frame();
    voxelPositions.clear();
}

void BgfxRenderer::drawVoxel(const WorldCoord& position) {
    voxelPositions.push_back(position);
}

void BgfxRenderer::setViewport(int width, int height) {
    viewWidth  = static_cast<uint32_t>(width);
    viewHeight = static_cast<uint32_t>(height);
    bgfx::reset(viewWidth, viewHeight, BGFX_RESET_VSYNC);
    bgfx::setViewRect(0, 0, 0, static_cast<uint16_t>(viewWidth), static_cast<uint16_t>(viewHeight));
}

void BgfxRenderer::setCameraPosition(const WorldCoord& pos) {
    cameraPos = pos;
}

void BgfxRenderer::setCameraRotation(float pitch, float yaw, float roll) {
    cameraRot = {pitch, yaw, roll};
}

void BgfxRenderer::cleanup() {
    if (bgfx::isValid(vbo)) { bgfx::destroy(vbo); vbo = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(ibo)) { bgfx::destroy(ibo); ibo = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(program)) { bgfx::destroy(program); program = BGFX_INVALID_HANDLE; }
}

void BgfxRenderer::shutdown() {
    if (!initialized) return;
    cleanup();
    bgfx::shutdown();
    initialized = false;
}

void BgfxRenderer::renderWorld(const World& /*world*/) {
    // Placeholder — full layer-aware world rendering is M2/M3.
}

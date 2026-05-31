#include "BgfxRenderer.h"
#include "ChunkMesh.h"
#include "Palette.h"
#include <bgfx/platform.h>
#include <iostream>
#include <cstring>

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

// Cube geometry — 8 vertices in unit cube centred at origin.
// Colors are patched per-voxel into a transient vertex buffer each frame so that
// different voxels can use different palette entries without changing this template.
static const VoxelVertex kCubeTemplate[8] = {
    {-0.5f, -0.5f,  0.5f, 0},
    { 0.5f, -0.5f,  0.5f, 0},
    { 0.5f,  0.5f,  0.5f, 0},
    {-0.5f,  0.5f,  0.5f, 0},
    {-0.5f, -0.5f, -0.5f, 0},
    { 0.5f, -0.5f, -0.5f, 0},
    { 0.5f,  0.5f, -0.5f, 0},
    {-0.5f,  0.5f, -0.5f, 0},
};

static const uint16_t kCubeIndices[36] = {
    0,1,2, 0,2,3,
    4,6,5, 4,7,6,
    0,4,5, 0,5,1,
    2,6,7, 2,7,3,
    0,3,7, 0,7,4,
    1,5,6, 1,6,2,
};

// The 12 edges of the cube template, as line-list index pairs. Used to draw the
// targeted-voxel highlight as a wireframe box.
static const uint16_t kCubeLineIndices[24] = {
    0,1, 1,2, 2,3, 3,0,   // +Z face loop
    4,5, 5,6, 6,7, 7,4,   // -Z face loop
    0,4, 1,5, 2,6, 3,7,   // verticals
};

bgfx::VertexLayout VoxelVertex::layout;

void VoxelVertex::initLayout() {
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)
        .end();
}

BgfxRenderer::BgfxRenderer()
    : program(BGFX_INVALID_HANDLE),
      ibo(BGFX_INVALID_HANDLE),
      lineIbo(BGFX_INVALID_HANDLE),
      cameraRot{0.0f, 0.0f, 0.0f},
      viewWidth(800),
      viewHeight(600),
      crosshair(false),
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
    ibo     = bgfx::createIndexBuffer(bgfx::makeRef(kCubeIndices, sizeof(kCubeIndices)));
    lineIbo = bgfx::createIndexBuffer(bgfx::makeRef(kCubeLineIndices, sizeof(kCubeLineIndices)));

    const bgfx::RendererType::Enum rendererType = bgfx::getRendererType();
    bgfx::ShaderHandle vsh = bgfx::createEmbeddedShader(s_embeddedShaders, rendererType, "vs_voxel");
    bgfx::ShaderHandle fsh = bgfx::createEmbeddedShader(s_embeddedShaders, rendererType, "fs_voxel");
    program = bgfx::createProgram(vsh, fsh, true);
    if (!bgfx::isValid(program))
        std::cerr << "[BgfxRenderer] Failed to create shader program\n";

    initialized = true;
}

void BgfxRenderer::render() {
    if (!initialized) return;

    // Per-frame view matrix: camera sits at the floating-origin (local 0,0,0).
    // All voxel positions are already translated into camera-local space via
    // toLocalFloat(cameraPos) below, so only orientation enters the view matrix.
    float sp = bx::sin(cameraRot.x), cp = bx::cos(cameraRot.x);
    float sy = bx::sin(cameraRot.y), cy = bx::cos(cameraRot.y);
    bx::Vec3 eye     = {0.0f, 0.0f, 0.0f};
    bx::Vec3 forward = {cp * sy, sp, cp * cy};
    bx::Vec3 at      = bx::add(eye, forward);
    float view[16];
    bx::mtxLookAt(view, eye, at);

    float proj[16];
    bx::mtxProj(proj, 60.0f,
                float(viewWidth) / float(viewHeight),
                0.01f, 1000.0f,
                bgfx::getCaps()->homogeneousDepth);

    // Two views share the camera transform and the back buffer (with its depth):
    //   view 0 — opaque geometry, depth write on (clears color+depth, set at init)
    //   view 1 — translucent geometry, alpha-blended, depth test on / write off,
    //            no cull, drawn after view 0 so water composites over the terrain.
    // View 1 deliberately has no clear, so it keeps view 0's color and depth.
    bgfx::setViewTransform(0, view, proj);
    bgfx::setViewRect(0, 0, 0, static_cast<uint16_t>(viewWidth), static_cast<uint16_t>(viewHeight));
    bgfx::setViewTransform(1, view, proj);
    bgfx::setViewRect(1, 0, 0, static_cast<uint16_t>(viewWidth), static_cast<uint16_t>(viewHeight));

    constexpr uint64_t kTranslucentState =
        BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ALPHA;

    bgfx::touch(0);

    for (const auto& pv : pendingVoxels) {
        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb, 8, VoxelVertex::layout);
        VoxelVertex* verts = reinterpret_cast<VoxelVertex*>(tvb.data);
        std::memcpy(verts, kCubeTemplate, sizeof(kCubeTemplate));
        for (int i = 0; i < 8; ++i)
            verts[i].abgr = pv.abgr;

        // Floating origin: convert world-space position to camera-local float.
        // This keeps vertex values small in magnitude, preserving float32 precision
        // even at large world scales. See docs/ARCHITECTURE.md §9.
        glm::vec3 lp = pv.pos.toLocalFloat(cameraPos);
        float mtx[16];
        bx::mtxTranslate(mtx, lp.x, lp.y, lp.z);

        bgfx::setState(BGFX_STATE_DEFAULT);
        bgfx::setTransform(mtx);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(ibo);
        if (bgfx::isValid(program))
            bgfx::submit(0, program);
    }

    // Per-chunk static meshes: one draw call per non-empty batch, placed via a
    // floating-origin model transform of the chunk's world origin (ARCHITECTURE
    // §9). Opaque batch on view 0; translucent (water) batch on view 1.
    for (const auto& pc : pendingChunks) {
        glm::vec3 lo = pc.origin.toLocalFloat(cameraPos);
        float mtx[16];
        bx::mtxTranslate(mtx, lo.x, lo.y, lo.z);

        if (bgfx::isValid(pc.opaqueIbh) && bgfx::isValid(program)) {
            bgfx::setState(BGFX_STATE_DEFAULT);
            bgfx::setTransform(mtx);
            bgfx::setVertexBuffer(0, pc.vbh);
            bgfx::setIndexBuffer(pc.opaqueIbh);
            bgfx::submit(0, program);
        }

        if (bgfx::isValid(pc.translucentIbh) && bgfx::isValid(program)) {
            bgfx::setState(kTranslucentState);
            bgfx::setTransform(mtx);
            bgfx::setVertexBuffer(0, pc.vbh);
            bgfx::setIndexBuffer(pc.translucentIbh);
            bgfx::submit(1, program);
        }
    }

    // Targeted-voxel highlights: the cube template drawn as a line list, scaled to
    // the voxel size and centered on the voxel. Depth-tested (LEQUAL) so edges on
    // the block surface still show, with no depth write so the outline never
    // occludes geometry. Drawn on view 0 after the opaque pass.
    constexpr uint64_t kLineState =
        BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES;
    for (const auto& ph : pendingHighlights) {
        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb, 8, VoxelVertex::layout);
        VoxelVertex* verts = reinterpret_cast<VoxelVertex*>(tvb.data);
        std::memcpy(verts, kCubeTemplate, sizeof(kCubeTemplate));
        for (int i = 0; i < 8; ++i)
            verts[i].abgr = ph.abgr;

        glm::vec3 lp = ph.center.toLocalFloat(cameraPos);
        // Scale the unit (±0.5) template up to the voxel size, nudged out slightly
        // so the wireframe sits just outside the block faces (avoids z-fighting).
        float mtx[16];
        bx::mtxSRT(mtx, ph.size * 1.02f, ph.size * 1.02f, ph.size * 1.02f,
                   0.0f, 0.0f, 0.0f, lp.x, lp.y, lp.z);

        if (bgfx::isValid(program)) {
            bgfx::setState(kLineState);
            bgfx::setTransform(mtx);
            bgfx::setVertexBuffer(0, &tvb);
            bgfx::setIndexBuffer(lineIbo);
            bgfx::submit(0, program);
        }
    }

    // Crosshair: a centered '+' via bgfx debug text (8x16 character cells).
    bgfx::setDebug(crosshair ? BGFX_DEBUG_TEXT : BGFX_DEBUG_NONE);
    if (crosshair) {
        bgfx::dbgTextClear();
        const uint16_t chX = static_cast<uint16_t>((viewWidth  / 8) / 2);
        const uint16_t chY = static_cast<uint16_t>((viewHeight / 16) / 2);
        bgfx::dbgTextPrintf(chX, chY, 0x0f, "+");
    }

    pendingVoxels.clear();
    pendingChunks.clear();
    pendingHighlights.clear();
    bgfx::frame();
}

void BgfxRenderer::drawVoxel(const WorldCoord& position, uint32_t abgr) {
    pendingVoxels.push_back({position, abgr});
}

void BgfxRenderer::renderWorld(const World& world) {
    for (int z = 0; z < world.getDepth(); ++z) {
        for (int y = 0; y < world.getHeight(); ++y) {
            for (int x = 0; x < world.getWidth(); ++x) {
                const Voxel& v = world.getVoxel(x, y, z);
                if (!v.isEmpty())
                    drawVoxel(WorldCoord(double(x), double(y), double(z)),
                              palette::color(v.material.palette_index));
            }
        }
    }
}

void BgfxRenderer::renderChunk(const ChunkMesh& mesh, const WorldCoord& chunkOrigin) {
    if (mesh.empty()) return;
    pendingChunks.push_back({chunkOrigin, mesh.vbh(), mesh.opaqueIbh(), mesh.translucentIbh()});
}

void BgfxRenderer::drawVoxelHighlight(const WorldCoord& center, float size, uint32_t abgr) {
    pendingHighlights.push_back({center, size, abgr});
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
    if (bgfx::isValid(ibo))     { bgfx::destroy(ibo);     ibo     = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(lineIbo)) { bgfx::destroy(lineIbo); lineIbo = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(program)) { bgfx::destroy(program); program = BGFX_INVALID_HANDLE; }
}

void BgfxRenderer::shutdown() {
    if (!initialized) return;
    cleanup();
    bgfx::shutdown();
    initialized = false;
}

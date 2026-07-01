#pragma once

#include "renderer/Renderer.h"
#include "renderer/Frustum.h"
#include "../world/World.h"
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <string>
#include <vector>

class ChunkMesh;

// Cell-grid HUD overlay helpers (M17 C2). bgfx's built-in debug text draws an
// 8x16 grid of character cells, each with a one-byte color attribute: the top
// 4 bits select a background color and the bottom 4 a foreground color, both
// indices into bgfx's 16-color VGA/ANSI palette. attr() packs a (fg, bg) pair
// into that byte so HUD call sites read clearly; the named constants cover the
// palette a demo HUD actually reaches for.
namespace hud {
enum Color : uint8_t {
    Black = 0,  Blue = 1,       Green = 2,      Cyan = 3,
    Red = 4,    Magenta = 5,    Brown = 6,      LightGray = 7,
    DarkGray = 8, LightBlue = 9, LightGreen = 10, LightCyan = 11,
    LightRed = 12, LightMagenta = 13, Yellow = 14, White = 15,
};
inline uint8_t attr(uint8_t fg, uint8_t bg = Black) {
    return static_cast<uint8_t>((bg << 4) | (fg & 0x0f));
}
}  // namespace hud

// GPU vertex for the voxel program. Memory layout MUST match ChunkMeshData's
// MeshVertex (position + packed ABGR color + tile-local atlas UV + the bound
// tile's atlas sub-rect): ChunkMesh uploads MeshVertex memory through this layout,
// and the per-voxel/highlight transient buffers below memcpy kCubeTemplate
// (VoxelVertex) into the same layout. r0..r3 (TexCoord1) carry the tile sub-rect
// (u0,v0,u1,v1) the fragment shader wraps the in-tile UV into (M15 T5).
struct VoxelVertex {
    float    x, y, z;
    uint32_t abgr;
    float    u, v;
    float    r0, r1, r2, r3;
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
    void setCameraUp(const glm::vec3& worldUp) override;
    void setFog(const FogParams& fog) override;
    void setClearColor(const glm::vec3& rgb) override;
    void cleanup() override;
    void shutdown() override;

    // Submit all non-empty voxels in the world using palette-mapped colors.
    void renderWorld(const World& world);

    // Submit a pre-built chunk mesh, placed via a floating-origin model transform
    // of the chunk's world-space origin. Reuses the voxel shader program. The mesh
    // is in chunk-local units (1 voxel == 1 unit); voxelSizeM scales it to the
    // layer's world scale, so composite/immutable layers render at their own size.
    //
    // The frustum-culling bounding sphere is derived from the mesh's own captured
    // chunk extent (ChunkMesh::sizeVoxels) times voxelSizeM — the caller cannot
    // supply a mismatched extent, so culling can never shrink below the geometry.
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

    // Install the texture atlas sampled by the voxel fragment shader (M15 T2/T3).
    // Pass a valid handle to bind a content atlas built by the texture pipeline;
    // pass BGFX_INVALID_HANDLE to revert to the built-in 1×1 white tile, which
    // makes sampling a no-op so colored (untextured) worlds render unchanged. The
    // renderer does NOT take ownership — the texture pipeline owns the atlas
    // lifetime (the §8 owner-tracked teardown); the renderer only references it.
    void setAtlas(bgfx::TextureHandle atlas) { atlasTex = atlas; }

    // Number of draw calls submitted in the most recent frame (via bgfx stats).
    uint32_t drawCallCount() const { return lastDrawCalls; }

    // Enable/disable a centered crosshair drawn via bgfx debug text.
    void setCrosshair(bool enabled) { crosshair = enabled; }

    // Replace the top-left HUD overlay, one string per line, drawn via bgfx debug
    // text. Pass an empty vector to clear it. Persists across frames until changed.
    void setHudText(std::vector<std::string> lines) { hudLines = std::move(lines); }

    // ── Cell-grid HUD overlay (M17 C2) ──────────────────────────────────────
    // An immediate-mode 2D overlay for composing a real game HUD (health bars,
    // inventory slots, a minimap) over the scene. It shares the same bgfx 8x16
    // debug-text cells as setHudText()/the crosshair, but exposes them as a
    // general draw list a game rebuilds every frame. Coordinates are in CELLS
    // (col, row), origin top-left; the live grid is hudCols()xhudRows(). Colors
    // are 4-bit indices into bgfx's debug palette — pack a cell attribute with
    // hud::attr(fg, bg).
    //
    // Usage: hudClear() once at the top of the frame, then the draw calls, then
    // render() rasterizes the list into the shared debug-text pass and clears it.
    // Independent of setHudText() (whose persistent lines still draw).
    void hudClear() { hudCmds.clear(); }
    // Draw a string starting at (col, row) with one color attribute.
    void hudText(int col, int row, uint8_t attr, const std::string& text);
    // Fill a col x row block of w x h cells with a single glyph + attribute
    // (default a colored space) — the building block for bars and panels.
    void hudFill(int col, int row, int w, int h, uint8_t attr, char glyph = ' ');
    // Blit a w x h block of pre-built cells (2 bytes each: glyph, attribute),
    // row-major — used for the minimap. `cells` is copied; the caller's buffer
    // need not outlive the call.
    void hudCells(int col, int row, int w, int h, const uint8_t* cells);
    // Current overlay grid size in cells, for positioning bottom/right elements.
    int hudCols() const { return static_cast<int>(viewWidth  / 8); }
    int hudRows() const { return static_cast<int>(viewHeight / 16); }

private:
    struct PendingVoxel {
        WorldCoord pos;
        uint32_t   abgr;
    };

    struct PendingChunk {
        WorldCoord               origin;
        double                   voxelSizeM;
        int                      sizeVoxels;  // chunk edge in voxels, from the mesh
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
    bgfx::UniformHandle       atlasSampler;  // s_atlas: the fragment shader's texture sampler
    bgfx::UniformHandle       fogColorU;     // u_fogColor: rgb of the distance-obscurance fog
    bgfx::UniformHandle       fogParamsU;    // u_fogParams: (near, far, density, 0)
    bgfx::TextureHandle       whiteTex;      // built-in 1×1 white tile (default atlas; engine-owned)
    bgfx::TextureHandle       atlasTex;      // currently bound atlas; defaults to whiteTex (not owned)
    std::vector<std::string>  hudLines;  // top-left debug-text overlay

    // Immediate-mode cell-grid overlay commands (M17 C2), rasterized and cleared
    // each frame in render(). See the hud* public methods above.
    struct HudCmd {
        enum Kind { Text, Fill, Cells } kind;
        int     col, row, w, h;
        uint8_t attr;
        char    glyph;
        std::string          text;   // Text
        std::vector<uint8_t> cells;  // Cells (2 bytes per cell: glyph, attr)
    };
    std::vector<HudCmd>       hudCmds;
    WorldCoord                cameraPos;
    bx::Vec3                  cameraRot; // {pitch, yaw, roll} in radians
    bx::Vec3                  cameraUp;  // world-space up the camera aligns to (default +Y)
    FogParams                 fog;       // distance-obscurance policy (default density 0 = off)
    uint32_t                  clearColor;// framebuffer clear color, packed RGBA (default 0x303030ff)
    uint32_t                  viewWidth;
    uint32_t                  viewHeight;
    float                     farClip;
    uint32_t                  lastDrawCalls;
    bool                      crosshair;
    bool                      initialized;
};

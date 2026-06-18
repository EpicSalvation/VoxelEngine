#pragma once

#include <bgfx/bgfx.h>

#include "world/Chunk.h"

// GPU-resident mesh for a single chunk: one static vertex buffer + one static
// index buffer, built once when the chunk loads and destroyed when it unloads.
// The vertex data is produced headlessly by buildChunkMeshData (see
// ChunkMeshData.h); this wrapper only owns the bgfx handles. Geometry is in
// chunk-local space — the renderer applies the chunk's world origin as a
// floating-origin model transform at submit time (BgfxRenderer::renderChunk).
class ChunkMesh {
public:
    ChunkMesh() = default;

    // Builds and uploads the mesh for chunk. An empty chunk yields an empty mesh
    // (no buffers, empty() == true). The geometry is split into an opaque batch
    // and a translucent batch (e.g. water) sharing one vertex buffer; either may
    // be empty. Requires VoxelVertex::layout to be initialized (done by
    // BgfxRenderer::initialize).
    //
    // voxelSizeM is the layer's voxel edge length in meters; it scales textured
    // tile-repeat UVs so a material tiles at a fixed world density regardless of
    // voxel size (M15 T5). The default 1.0 matches the single-scale callers; a
    // composite/immutable layer passes its own voxelSizeM (the same value it hands
    // BgfxRenderer::renderChunk).
    static ChunkMesh build(const Chunk& chunk, double voxelSizeM = 1.0);

    void destroy();
    bool empty() const { return opaqueCount_ == 0 && translucentCount_ == 0; }

    bgfx::VertexBufferHandle vbh()              const { return vbh_; }
    bgfx::IndexBufferHandle  opaqueIbh()        const { return opaqueIbh_; }
    bgfx::IndexBufferHandle  translucentIbh()   const { return translucentIbh_; }
    uint32_t                 opaqueCount()      const { return opaqueCount_; }
    uint32_t                 translucentCount() const { return translucentCount_; }

private:
    bgfx::VertexBufferHandle vbh_            = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  opaqueIbh_      = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  translucentIbh_ = BGFX_INVALID_HANDLE;
    uint32_t                 opaqueCount_      = 0;
    uint32_t                 translucentCount_ = 0;
};

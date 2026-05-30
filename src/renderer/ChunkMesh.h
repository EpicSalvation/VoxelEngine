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
    // (no buffers, empty() == true). Requires VoxelVertex::layout to be
    // initialized (done by BgfxRenderer::initialize).
    static ChunkMesh build(const Chunk& chunk);

    void destroy();
    bool empty() const { return indexCount_ == 0; }

    bgfx::VertexBufferHandle vbh()        const { return vbh_; }
    bgfx::IndexBufferHandle  ibh()        const { return ibh_; }
    uint32_t                 indexCount() const { return indexCount_; }

private:
    bgfx::VertexBufferHandle vbh_ = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  ibh_ = BGFX_INVALID_HANDLE;
    uint32_t                 indexCount_ = 0;
};

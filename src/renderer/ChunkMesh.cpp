#include "ChunkMesh.h"
#include "ChunkMeshData.h"
#include "BgfxRenderer.h"  // for VoxelVertex::layout (matches MeshVertex memory layout)

#include <vector>

ChunkMesh ChunkMesh::build(const Chunk& chunk, double voxelSizeM) {
    // Reuse thread-local scratch across calls instead of allocating fresh vectors
    // for every chunk. buildChunkMeshData clears them at entry, so .clear() keeps
    // the capacity grown by earlier builds and steady-state meshing never
    // reallocates — the M17 profiling pass measured this allocation churn at ~30%
    // of light-chunk and ~45% of dense-chunk CPU mesh-build time. thread_local
    // keeps it safe if meshing is ever driven off more than one thread; the bgfx
    // upload below copies out of the scratch, so reusing it is sound.
    thread_local std::vector<MeshVertex> verts;
    thread_local std::vector<uint32_t>   opaqueIdx;
    thread_local std::vector<uint32_t>   translucentIdx;
    buildChunkMeshData(chunk, verts, opaqueIdx, translucentIdx, voxelSizeM);

    ChunkMesh mesh;
    mesh.sizeVoxels_ = chunk.size();  // authoritative culling extent (see header)
    if (verts.empty())
        return mesh;  // empty mesh; no buffers created

    // One vertex buffer shared by both index batches. Copy into bgfx-owned
    // memory so the local vectors can be freed immediately.
    const bgfx::Memory* vmem =
        bgfx::copy(verts.data(), static_cast<uint32_t>(verts.size() * sizeof(MeshVertex)));
    mesh.vbh_ = bgfx::createVertexBuffer(vmem, VoxelVertex::layout);

    if (!opaqueIdx.empty()) {
        const bgfx::Memory* imem =
            bgfx::copy(opaqueIdx.data(), static_cast<uint32_t>(opaqueIdx.size() * sizeof(uint32_t)));
        mesh.opaqueIbh_   = bgfx::createIndexBuffer(imem, BGFX_BUFFER_INDEX32);
        mesh.opaqueCount_ = static_cast<uint32_t>(opaqueIdx.size());
    }

    if (!translucentIdx.empty()) {
        const bgfx::Memory* imem =
            bgfx::copy(translucentIdx.data(), static_cast<uint32_t>(translucentIdx.size() * sizeof(uint32_t)));
        mesh.translucentIbh_   = bgfx::createIndexBuffer(imem, BGFX_BUFFER_INDEX32);
        mesh.translucentCount_ = static_cast<uint32_t>(translucentIdx.size());
    }

    return mesh;
}

void ChunkMesh::destroy() {
    if (bgfx::isValid(vbh_))            { bgfx::destroy(vbh_);            vbh_            = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(opaqueIbh_))      { bgfx::destroy(opaqueIbh_);      opaqueIbh_      = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(translucentIbh_)) { bgfx::destroy(translucentIbh_); translucentIbh_ = BGFX_INVALID_HANDLE; }
    opaqueCount_      = 0;
    translucentCount_ = 0;
}

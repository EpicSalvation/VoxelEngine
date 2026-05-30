#include "ChunkMesh.h"
#include "ChunkMeshData.h"
#include "BgfxRenderer.h"  // for VoxelVertex::layout (matches MeshVertex memory layout)

#include <vector>

ChunkMesh ChunkMesh::build(const Chunk& chunk) {
    std::vector<MeshVertex> verts;
    std::vector<uint32_t>   idx;
    buildChunkMeshData(chunk, verts, idx);

    ChunkMesh mesh;
    if (idx.empty())
        return mesh;  // empty mesh; no buffers created

    // Copy into bgfx-owned memory so the local vectors can be freed immediately.
    const bgfx::Memory* vmem =
        bgfx::copy(verts.data(), static_cast<uint32_t>(verts.size() * sizeof(MeshVertex)));
    const bgfx::Memory* imem =
        bgfx::copy(idx.data(), static_cast<uint32_t>(idx.size() * sizeof(uint32_t)));

    mesh.vbh_ = bgfx::createVertexBuffer(vmem, VoxelVertex::layout);
    mesh.ibh_ = bgfx::createIndexBuffer(imem, BGFX_BUFFER_INDEX32);
    mesh.indexCount_ = static_cast<uint32_t>(idx.size());
    return mesh;
}

void ChunkMesh::destroy() {
    if (bgfx::isValid(vbh_)) { bgfx::destroy(vbh_); vbh_ = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(ibh_)) { bgfx::destroy(ibh_); ibh_ = BGFX_INVALID_HANDLE; }
    indexCount_ = 0;
}

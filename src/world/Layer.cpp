#include "Layer.h"

Layer::Layer(const LayerDef& def)
    : name_(def.name),
      mode_(def.mode),
      voxelSizeM_(def.voxel_size_m),
      chunkSizeVoxels_(def.chunk_size_voxels),
      decomposeTo_(def.decompose_to) {}

Chunk* Layer::loadChunk(ChunkCoord coord, LayerGeneratorFn generator, void* user_data) {
    auto it = chunks_.find(coord);
    if (it != chunks_.end())
        return it->second.get();

    WorldCoord origin = chunkmath::chunkOrigin(coord, voxelSizeM_, chunkSizeVoxels_);
    auto chunk = std::make_unique<Chunk>(coord, chunkSizeVoxels_, origin);
    if (generator)
        generator(origin, chunkSizeVoxels_, chunk->data(), user_data);

    Chunk* raw = chunk.get();
    chunks_.emplace(coord, std::move(chunk));
    return raw;
}

void Layer::unloadChunk(ChunkCoord coord) {
    chunks_.erase(coord);
}

Chunk* Layer::insertChunk(std::unique_ptr<Chunk> chunk) {
    if (!chunk) return nullptr;
    Chunk* raw = chunk.get();
    chunks_[chunk->coord()] = std::move(chunk);
    return raw;
}

const Chunk* Layer::getChunk(ChunkCoord coord) const {
    auto it = chunks_.find(coord);
    return it == chunks_.end() ? nullptr : it->second.get();
}

Voxel Layer::getVoxel(const WorldCoord& pos) const {
    const chunkmath::VoxelCoord v = chunkmath::worldToVoxel(pos, voxelSizeM_);
    const chunkmath::LocalVoxel lv = chunkmath::voxelToChunkLocal(v, chunkSizeVoxels_);
    const Chunk* chunk = getChunk(lv.chunk);
    if (!chunk) return Voxel::empty();
    return chunk->at(lv.x, lv.y, lv.z);
}

bool Layer::setVoxel(const WorldCoord& pos, const Voxel& voxel) {
    const chunkmath::VoxelCoord v = chunkmath::worldToVoxel(pos, voxelSizeM_);
    const chunkmath::LocalVoxel lv = chunkmath::voxelToChunkLocal(v, chunkSizeVoxels_);
    auto it = chunks_.find(lv.chunk);
    if (it == chunks_.end()) return false;
    it->second->at(lv.x, lv.y, lv.z) = voxel;
    it->second->markDirty();
    return true;
}

bool Layer::isChunkDirty(ChunkCoord coord) const {
    auto it = chunks_.find(coord);
    return it != chunks_.end() && it->second->dirty();
}

std::vector<ChunkCoord> Layer::dirtyChunkCoords() const {
    std::vector<ChunkCoord> out;
    for (const auto& kv : chunks_)
        if (kv.second->dirty())
            out.push_back(kv.first);
    return out;
}

void Layer::clearChunkDirty(ChunkCoord coord) {
    auto it = chunks_.find(coord);
    if (it != chunks_.end())
        it->second->clearDirty();
}

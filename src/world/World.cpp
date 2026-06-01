#include "World.h"
#include <algorithm>
#include <iostream>

World::World(int width, int height, int depth)
    : width_(width), height_(height), depth_(depth),
      voxels_(static_cast<size_t>(width * height * depth), Voxel::empty())
{}

World::World(const LayerDef& layer)
    : width_(0), height_(0), depth_(0)
{
    layers_.push_back(std::make_unique<Layer>(layer));
    primary_ = layers_.front().get();
}

World::World(const LayerConfig& config)
    : width_(0), height_(0), depth_(0)
{
    for (const LayerDef& def : config.layers())
        layers_.push_back(std::make_unique<Layer>(def));
    // The single-layer forwarding API (edit, pick, collision substep scale,
    // persistence) targets the terminal layer — the one the player modifies.
    // Configs are ordered coarse→fine, so the terminal layer is not first; fall
    // back to the first layer for a stack without a terminal layer.
    for (auto& l : layers_)
        if (l->mode() == VoxelMode::terminal) { primary_ = l.get(); break; }
    if (!primary_ && !layers_.empty())
        primary_ = layers_.front().get();
}

// ── Layer stack ────────────────────────────────────────────────────────────

Layer* World::layer(const std::string& name) {
    for (auto& l : layers_)
        if (l->name() == name) return l.get();
    return nullptr;
}

const Layer* World::layer(const std::string& name) const {
    for (const auto& l : layers_)
        if (l->name() == name) return l.get();
    return nullptr;
}

Layer* World::childLayer(const Layer& parent) {
    if (!parent.decomposeTo()) return nullptr;
    return layer(*parent.decomposeTo());
}

const Layer* World::childLayer(const Layer& parent) const {
    if (!parent.decomposeTo()) return nullptr;
    return layer(*parent.decomposeTo());
}

bool World::anySolidAt(const WorldCoord& pos) const {
    for (const auto& l : layers_)
        if (!l->getVoxel(pos).isEmpty()) return true;
    return false;
}

// ── Single-layer chunked API (forwards to the primary layer) ────────────────

Chunk* World::loadChunk(ChunkCoord coord, LayerGeneratorFn generator, void* user_data) {
    return primary_ ? primary_->loadChunk(coord, generator, user_data) : nullptr;
}

void World::unloadChunk(ChunkCoord coord) {
    if (primary_) primary_->unloadChunk(coord);
}

Chunk* World::insertChunk(std::unique_ptr<Chunk> chunk) {
    return primary_ ? primary_->insertChunk(std::move(chunk)) : nullptr;
}

const Chunk* World::getChunk(ChunkCoord coord) const {
    return primary_ ? primary_->getChunk(coord) : nullptr;
}

Voxel World::getVoxel(const WorldCoord& pos) const {
    return primary_ ? primary_->getVoxel(pos) : Voxel::empty();
}

bool World::setVoxel(const WorldCoord& pos, const Voxel& voxel) {
    return primary_ ? primary_->setVoxel(pos, voxel) : false;
}

bool World::isChunkDirty(ChunkCoord coord) const {
    return primary_ ? primary_->isChunkDirty(coord) : false;
}

std::vector<ChunkCoord> World::dirtyChunkCoords() const {
    return primary_ ? primary_->dirtyChunkCoords() : std::vector<ChunkCoord>{};
}

void World::clearChunkDirty(ChunkCoord coord) {
    if (primary_) primary_->clearChunkDirty(coord);
}

const World::ChunkStore& World::chunks() const {
    static const ChunkStore kEmpty;
    return primary_ ? primary_->chunks() : kEmpty;
}

double World::voxelSizeM() const {
    return primary_ ? primary_->voxelSizeM() : 1.0;
}

int World::chunkSizeVoxels() const {
    return primary_ ? primary_->chunkSizeVoxels() : 32;
}

// ── Finite flat-grid API (M1/M2) ────────────────────────────────────────────

void World::generateWorld(LayerGeneratorFn generator, void* user_data) {
    if (!generator) return;
    int grid = std::min({width_, height_, depth_});
    std::vector<Voxel> chunk(static_cast<size_t>(grid * grid * grid));
    generator(WorldCoord{}, grid, chunk.data(), user_data);
    for (int z = 0; z < grid; ++z)
        for (int y = 0; y < grid; ++y)
            for (int x = 0; x < grid; ++x)
                setVoxel(x, y, z, chunk[x + grid * (y + grid * z)]);
}

Voxel World::getVoxel(int x, int y, int z) const {
    if (x < 0 || x >= width_ || y < 0 || y >= height_ || z < 0 || z >= depth_) {
        std::cerr << "[World] getVoxel out of bounds: " << x << " " << y << " " << z << "\n";
        return Voxel::empty();
    }
    return voxels_[idx(x, y, z)];
}

void World::setVoxel(int x, int y, int z, const Voxel& voxel) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_ || z < 0 || z >= depth_) {
        std::cerr << "[World] setVoxel out of bounds: " << x << " " << y << " " << z << "\n";
        return;
    }
    voxels_[idx(x, y, z)] = voxel;
}

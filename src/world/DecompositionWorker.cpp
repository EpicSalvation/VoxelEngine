#include "DecompositionWorker.h"

#include <algorithm>
#include <utility>

DecompositionWorker::DecompositionWorker(unsigned threadCount) {
    if (threadCount == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        threadCount = hw == 0 ? 2u : std::min(hw, 8u);
    }
    threads_.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i)
        threads_.emplace_back([this] { workerLoop(); });
}

DecompositionWorker::~DecompositionWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : threads_)
        if (t.joinable()) t.join();
}

void DecompositionWorker::enqueue(DecompositionJob job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push(std::move(job));
    }
    inFlight_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
}

std::vector<DecompositionResult> DecompositionWorker::drain() {
    std::vector<DecompositionResult> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.swap(results_);
    return out;
}

std::unique_ptr<Chunk> DecompositionWorker::generateChunk(ChunkCoord coord,
                                                          int chunkSize,
                                                          double voxelSizeM,
                                                          LayerGeneratorFn generator,
                                                          void* userData) {
    const WorldCoord origin = chunkmath::chunkOrigin(coord, voxelSizeM, chunkSize);
    auto chunk = std::make_unique<Chunk>(coord, chunkSize, origin);
    if (generator)
        generator(origin, chunkSize, chunk->data(), userData);
    return chunk;
}

std::unique_ptr<Chunk> DecompositionWorker::generateChunkFromRecipe(
        ChunkCoord coord, int chunkSize, double voxelSizeM,
        const ResolvedRecipe& recipe, chunkmath::VoxelCoord macroChildMin,
        int64_t ratio, uint64_t seed) {
    const WorldCoord origin = chunkmath::chunkOrigin(coord, voxelSizeM, chunkSize);
    auto chunk = std::make_unique<Chunk>(coord, chunkSize, origin);
    fillChildChunk(*chunk, voxelSizeM, recipe, macroChildMin, ratio, seed);
    return chunk;
}

void DecompositionWorker::workerLoop() {
    for (;;) {
        DecompositionJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_ && jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop();
        }

        DecompositionResult result;
        result.macro     = job.macro;
        result.layerName = job.layerName;
        result.chunks.reserve(job.childChunks.size());
        for (ChunkCoord c : job.childChunks) {
            if (job.recipe) {
                result.chunks.push_back(generateChunkFromRecipe(
                    c, job.childChunkSize, job.childVoxelSizeM, *job.recipe,
                    job.macroChildMin, job.ratio, job.seed));
            } else {
                result.chunks.push_back(generateChunk(c, job.childChunkSize,
                                                      job.childVoxelSizeM,
                                                      job.generator, job.userData));
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            results_.push_back(std::move(result));
        }
        inFlight_.fetch_sub(1, std::memory_order_relaxed);
    }
}

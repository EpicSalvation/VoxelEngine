#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "Chunk.h"
#include "ChunkCoordMath.h"
#include "plugin_api.h"  // LayerGeneratorFn

// Async on-demand decomposition (M6, docs/ARCHITECTURE.md §4).
//
// When a composite macro voxel must reveal its interior, its child (decompose_to)
// layer's chunks covering the macro voxel's world-space subvolume are generated
// on a worker thread and handed back to the main thread, which inserts them into
// the child layer's chunk store. The main thread never blocks: until the result
// arrives it keeps rendering the atomic block (pop-in is expected, not a bug).
//
// A job is a pure function of its inputs — the child layer's generator is
// deterministic (no rand/time/unordered iteration, per the architecture rule) —
// so the worker adds no nondeterminism: the same job yields the same child grid
// on any thread, every run.

struct DecompositionJob {
    chunkmath::VoxelCoord   macro;            // composite-layer voxel (bookkeeping key)
    std::vector<ChunkCoord> childChunks;      // child-layer chunks to generate
    int                     childChunkSize  = 0;
    double                  childVoxelSizeM = 0.0;
    LayerGeneratorFn        generator       = nullptr;
    void*                   userData        = nullptr;
};

struct DecompositionResult {
    chunkmath::VoxelCoord               macro;
    std::vector<std::unique_ptr<Chunk>> chunks;
};

class DecompositionWorker {
public:
    // threadCount == 0 picks a sensible default from hardware_concurrency.
    explicit DecompositionWorker(unsigned threadCount = 0);
    ~DecompositionWorker();

    DecompositionWorker(const DecompositionWorker&)            = delete;
    DecompositionWorker& operator=(const DecompositionWorker&) = delete;

    // Queue a job for a worker thread. Returns immediately.
    void enqueue(DecompositionJob job);

    // Move out every result completed since the last call. Called on the main
    // thread; never blocks on workers (returns empty if nothing is ready).
    std::vector<DecompositionResult> drain();

    // Jobs queued or running but not yet drained.
    size_t inFlight() const { return static_cast<size_t>(inFlight_.load()); }

    unsigned threadCount() const { return static_cast<unsigned>(threads_.size()); }

    // Build one chunk at coord and run the generator over it. Pure (no shared
    // state); the unit of work a job is made of, and used directly by tests.
    static std::unique_ptr<Chunk> generateChunk(ChunkCoord coord, int chunkSize,
                                                 double voxelSizeM,
                                                 LayerGeneratorFn generator,
                                                 void* userData);

private:
    void workerLoop();

    std::vector<std::thread>         threads_;
    mutable std::mutex               mutex_;
    std::condition_variable          cv_;
    std::queue<DecompositionJob>     jobs_;
    std::vector<DecompositionResult> results_;
    std::atomic<int>                 inFlight_{0};
    bool                             stop_ = false;
};

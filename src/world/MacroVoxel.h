#pragma once

#include <unordered_set>

#include "ChunkCoordMath.h"

// Decomposition state for a composite layer (M6).
//
// A composite layer's voxels are large "macro voxels". Until something needs a
// macro voxel's interior it stays atomic — rendered as a single solid block of
// its surface material. On demand it *decomposes*: the child (decompose_to) layer
// generates the voxel grid filling the macro voxel's world-space subvolume, and
// from then on the child voxels are what render and collide in its place.
//
// DecompositionState is the bookkeeping that makes this idempotent and async-safe.
// A macro voxel (identified by its global VoxelCoord in the composite layer's
// grid) is in exactly one of three states:
//   - none      — atomic; renders as a block; eligible to decompose
//   - pending   — a DecompositionWorker job is in flight; do not re-enqueue
//   - decomposed— the child grid is resident; render the child layer instead
//
// This holds no recipe yet (recipes are M9); for M6 decomposition simply runs the
// child layer's generator over the subvolume. See DecompositionWorker.
class DecompositionState {
public:
    bool isDecomposed(chunkmath::VoxelCoord macro) const {
        return decomposed_.count(macro) != 0;
    }
    bool isPending(chunkmath::VoxelCoord macro) const {
        return pending_.count(macro) != 0;
    }
    // True when the macro voxel is atomic and not already being decomposed —
    // i.e. a decomposition could be enqueued for it.
    bool needsDecompose(chunkmath::VoxelCoord macro) const {
        return !isDecomposed(macro) && !isPending(macro);
    }

    // Mark a macro voxel as having an in-flight decomposition. Returns true if it
    // was newly marked (it was atomic); false if it was already pending or done,
    // so a caller can enqueue exactly once.
    bool markPending(chunkmath::VoxelCoord macro) {
        if (isDecomposed(macro) || isPending(macro)) return false;
        pending_.insert(macro);
        return true;
    }

    // Promote a pending macro voxel to decomposed (its child grid is now resident).
    void markDecomposed(chunkmath::VoxelCoord macro) {
        pending_.erase(macro);
        decomposed_.insert(macro);
    }

    // Forget a macro voxel entirely (e.g. its child grid was dropped), returning
    // it to the atomic state so it can decompose again later.
    void clear(chunkmath::VoxelCoord macro) {
        pending_.erase(macro);
        decomposed_.erase(macro);
    }

    size_t decomposedCount() const { return decomposed_.size(); }
    size_t pendingCount()    const { return pending_.size(); }

private:
    std::unordered_set<chunkmath::VoxelCoord, chunkmath::VoxelCoordHash> decomposed_;
    std::unordered_set<chunkmath::VoxelCoord, chunkmath::VoxelCoordHash> pending_;
};

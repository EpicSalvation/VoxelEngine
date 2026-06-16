#pragma once

#include <set>
#include <unordered_map>
#include <vector>

#include "simulation/NeighborWalk.h"
#include "world/ChunkCoordMath.h"

// FieldOverlay — the shared sparse field store M14's ThermalSystem and
// FluidSystem both sit on (docs/ARCHITECTURE.md §17).
//
// A coord -> float map with a configurable ambient/absent-cell default: only
// non-ambient cells are stored, so the overlay stays sparse and scoped to
// whatever region a solver has touched, never a dense per-chunk field (the
// engine's planetary-scale ambitions rule that out). It tracks its own
// active set (the non-ambient cells) and that set's 6-connected frontier —
// the candidate region a relaxation pass needs each tick — and iterates both
// in deterministic sorted-coord order (NeighborWalk.h's VoxelCoordLess), the
// same order the M13 support flood uses, so a solver built on top of this
// store stays byte-identical across runs (§4 determinism contract).
//
// FieldOverlay reads and writes only its own state; it holds no voxel data
// and never touches World. ThermalSystem and FluidSystem each own one
// instance (temperature, fluid amount) — two separate overlays, never fused,
// per the M14 "decoupled fields" design decision.

namespace sim {

class FieldOverlay {
public:
    explicit FieldOverlay(float ambient) : ambient_(ambient) {}

    float ambient() const { return ambient_; }

    // Current value at c, or ambient() if c is not in the active set.
    float get(const chunkmath::VoxelCoord& c) const {
        auto it = values_.find(c);
        return it != values_.end() ? it->second : ambient_;
    }

    bool isActive(const chunkmath::VoxelCoord& c) const { return values_.count(c) != 0; }

    // Set c's value. A value that lands back on ambient() drops the cell from
    // the active set instead of storing an explicit ambient entry, so a cell
    // that fully decays/drains stays out of the sparse store.
    void set(const chunkmath::VoxelCoord& c, float value) {
        if (value == ambient_) {
            values_.erase(c);
            active_.erase(c);
        } else {
            values_[c] = value;
            active_.insert(c);
        }
    }

    // Force c back to ambient and out of the active set, regardless of value
    // (used when a chunk evicts and its cells must not linger — §17 "scoped
    // to the resident region").
    void clear(const chunkmath::VoxelCoord& c) {
        values_.erase(c);
        active_.erase(c);
    }

    std::size_t activeCount() const { return active_.size(); }

    // The active set in deterministic sorted-coord order.
    std::vector<chunkmath::VoxelCoord> activeSorted() const {
        return {active_.begin(), active_.end()};
    }

    // The active set's 6-connected frontier — ambient neighbors of an active
    // cell — in deterministic sorted-coord order. This is the region a
    // relaxation pass must also consider: an ambient cell adjacent to an
    // active one may become active this tick.
    std::vector<chunkmath::VoxelCoord> frontierSorted() const {
        std::set<chunkmath::VoxelCoord, VoxelCoordLess> frontier;
        for (const chunkmath::VoxelCoord& c : active_)
            for (const chunkmath::VoxelCoord& n : neighbors6(c))
                if (!active_.count(n)) frontier.insert(n);
        return {frontier.begin(), frontier.end()};
    }

private:
    float ambient_;
    std::unordered_map<chunkmath::VoxelCoord, float, chunkmath::VoxelCoordHash> values_;
    std::set<chunkmath::VoxelCoord, VoxelCoordLess> active_;
};

}  // namespace sim

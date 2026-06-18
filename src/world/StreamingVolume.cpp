#include "StreamingVolume.h"

#include <algorithm>
#include <cstdlib>

namespace {

// Squared Euclidean chunk-distance between two chunk coords.
int64_t distSq(ChunkCoord a, ChunkCoord b) {
    const int64_t dx = a.x - b.x;
    const int64_t dy = a.y - b.y;
    const int64_t dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

bool StreamingVolume::contains(ChunkCoord center, ChunkCoord c) const {
    const int r = radiusChunks;
    if (r < 0) return false;

    switch (shape) {
        case StreamingShape::box: {
            // Isotropic 3D Chebyshev cube, clamped by the optional absolute Y band.
            // With the default unconstrained band this is a full cube of radius r
            // centered on the camera — no Y privilege — so a deep descent streams
            // downward with the camera instead of bottoming out on an absolute band.
            const int yLo = std::max(std::min(yMin, yMax), center.y - r);
            const int yHi = std::min(std::max(yMin, yMax), center.y + r);
            if (c.y < yLo || c.y > yHi) return false;
            return std::abs(c.x - center.x) <= r && std::abs(c.z - center.z) <= r;
        }
        case StreamingShape::sphere: {
            // Isotropic Euclidean ball: excludes the box corners (|d|=r√3 > r).
            return distSq(center, c) <= static_cast<int64_t>(r) * r;
        }
        case StreamingShape::shell: {
            // Thin Euclidean band [inner, outer] for a backdrop resident only at
            // range. inner = r − thickness, clamped at 0.
            const int inner = std::max(0, r - shellThicknessChunks);
            const int64_t d2 = distSq(center, c);
            return d2 <= static_cast<int64_t>(r) * r &&
                   d2 >= static_cast<int64_t>(inner) * inner;
        }
    }
    return false;
}

std::vector<ChunkCoord> StreamingVolume::desired(ChunkCoord center) const {
    std::vector<ChunkCoord> result;
    const int r = radiusChunks;
    if (r < 0) return result;

    if (shape == StreamingShape::box) {
        // Preserve the pre-M16 enumeration exactly (y outer, dz, dx inner) so a
        // box volume's desired set is byte-for-byte identical to the old LODManager.
        const int yLo = std::max(std::min(yMin, yMax), center.y - r);
        const int yHi = std::min(std::max(yMin, yMax), center.y + r);
        if (yLo > yHi) return result;
        result.reserve(static_cast<size_t>(2 * r + 1) * (2 * r + 1) * (yHi - yLo + 1));
        for (int y = yLo; y <= yHi; ++y)
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx)
                    result.push_back(ChunkCoord{center.x + dx, y, center.z + dz});
        return result;
    }

    // Sphere / shell: enumerate the bounding cube and keep what the predicate
    // admits. Same loop nesting as the box for a stable, deterministic order.
    for (int dy = -r; dy <= r; ++dy)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                const ChunkCoord c{center.x + dx, center.y + dy, center.z + dz};
                if (contains(center, c)) result.push_back(c);
            }
    return result;
}

StreamingVolume StreamingVolume::expandedBy(int margin) const {
    StreamingVolume v = *this;
    if (radiusChunks < 0) return v;  // empty volume (unknown layer) stays empty
    v.radiusChunks = radiusChunks + margin;
    if (shape == StreamingShape::shell)
        // Grow the outer edge by margin and shrink the inner edge by margin:
        // new inner = (r+margin) − (thickness+2·margin) = (r − thickness) − margin.
        v.shellThicknessChunks = shellThicknessChunks + 2 * margin;
    return v;
}

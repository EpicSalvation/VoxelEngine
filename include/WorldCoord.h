#pragma once

#include <glm/glm.hpp>

// World-space position type. Wraps glm::dvec3 (64-bit double precision).
//
// Rules (enforced by the type system):
//   - Use WorldCoord for ALL world-space positions in engine code and plugins.
//   - Never use raw float, double, glm::vec3, or glm::dvec3 for world-space positions.
//   - The only permitted conversion to single-precision float is toLocalFloat(), which
//     subtracts a camera origin first. Call it only in the renderer's GPU submission path.
//
// Why: a 32-bit float has ~7 significant decimal digits. At 100 km scale, sub-meter
// precision is completely lost. Fixing this after the fact requires auditing the entire
// codebase. Defining the type early costs almost nothing.
struct WorldCoord {
    glm::dvec3 value{0.0, 0.0, 0.0};

    WorldCoord() = default;
    explicit WorldCoord(double x, double y, double z) : value(x, y, z) {}
    explicit WorldCoord(const glm::dvec3& v) : value(v) {}

    WorldCoord operator+(const WorldCoord& rhs) const { return WorldCoord(value + rhs.value); }
    WorldCoord operator-(const WorldCoord& rhs) const { return WorldCoord(value - rhs.value); }
    WorldCoord operator*(double s)              const { return WorldCoord(value * s); }

    bool operator==(const WorldCoord& rhs) const { return value == rhs.value; }
    bool operator!=(const WorldCoord& rhs) const { return value != rhs.value; }

    // Convert to camera-local single-precision float for GPU submission only.
    // Subtracts cameraOrigin before narrowing, keeping the result small enough to be
    // precise in float32. Never call this for world-space arithmetic.
    glm::vec3 toLocalFloat(const WorldCoord& cameraOrigin) const {
        return glm::vec3(value - cameraOrigin.value);
    }
};

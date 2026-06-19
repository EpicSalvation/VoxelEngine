#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// axisrole — resolve a geometric voxel face to its gravity-relative ROLE
// (M16, G1/G2; docs/ARCHITECTURE.md §18).
//
// The appearance and decomposition tiers author faces by role — `up` (the
// surface skin / "top"), `down` ("bottom"), and `lateral` ("side") — rather than
// by a fixed +Y/-Y geometry. This is the shared seam that lets a grass block be
// grass-side-out on the +X face of an asteroid and a composite's decomposed
// crust be radial instead of a flat top slab. Both the mesh builder
// (materialfaces::faceTile) and the recipe boundary distribution
// (RecipeDesc::BoundaryDesc) resolve roles through this one function.
//
// The default gravity is constant -Y, under which `up` resolves to the +Y face
// and `down` to -Y — byte-identical to the pre-M16 Y-up convention, so every
// current world renders and decomposes unchanged.
// ---------------------------------------------------------------------------
namespace axisrole {

enum class Role { Up, Down, Lateral };

// Resolve a geometric face (its outward unit normal) against a gravity ("down")
// vector. The face whose normal most opposes gravity is Up; the face whose
// normal most aligns with gravity is Down; every other face is Lateral. A zero
// (or negligible) gravity vector has no up/down — every face is Lateral.
inline Role roleOf(const glm::dvec3& faceNormal, const glm::dvec3& gravityDir) {
    const double ax = std::abs(gravityDir.x);
    const double ay = std::abs(gravityDir.y);
    const double az = std::abs(gravityDir.z);
    const double maxc = std::max({ax, ay, az});
    if (maxc <= 1e-12) return Role::Lateral;  // zero-g: no privileged axis

    // Dominant gravity axis and the sign that points "down" along it.
    const int    axis  = (ax == maxc) ? 0 : (ay == maxc ? 1 : 2);
    const double gsign = (gravityDir[axis] < 0.0) ? -1.0 : 1.0;

    // A face not aligned with the dominant axis is lateral.
    if (std::abs(faceNormal[axis]) < 0.5) return Role::Lateral;

    const double fsign = (faceNormal[axis] < 0.0) ? -1.0 : 1.0;
    return (fsign == gsign) ? Role::Down : Role::Up;
}

}  // namespace axisrole

#pragma once

#include "WorldCoord.h"

#include <cmath>
#include <glm/glm.hpp>

// Conservative view-frustum test for chunk bounding spheres. Plane-distance
// tests against the bounding sphere err on the side of "visible" — this never
// culls geometry the renderer would actually draw, it only skips submitting
// chunks behind the camera or outside the view cone.
struct Frustum {
    glm::dvec3 pos{}, fwd{}, right{}, up{};
    double tanH = 0.0, tanV = 0.0, cosH = 1.0, cosV = 1.0, farClip = 0.0;

    void update(const WorldCoord& camPos, float pitch, float yaw,
                double aspect, double vfovDeg, double farClipM) {
        pos = camPos.value;
        const double cp = std::cos(pitch), sp = std::sin(pitch);
        const double cy = std::cos(yaw),   sy = std::sin(yaw);
        fwd   = glm::dvec3(cp * sy, sp, cp * cy);
        right = glm::dvec3(cy, 0.0, -sy);
        up    = glm::cross(fwd, right);
        tanV  = std::tan(glm::radians(vfovDeg) * 0.5);
        tanH  = tanV * aspect;
        cosV  = 1.0 / std::sqrt(1.0 + tanV * tanV);
        cosH  = 1.0 / std::sqrt(1.0 + tanH * tanH);
        farClip = farClipM;
    }

    bool sphereVisible(const glm::dvec3& center, double radius) const {
        const glm::dvec3 rel = center - pos;
        const double z = glm::dot(rel, fwd);
        if (z + radius < 0.0 || z - radius > farClip) return false;
        const double x = glm::dot(rel, right);
        if ((std::abs(x) - z * tanH) * cosH > radius) return false;
        const double y = glm::dot(rel, up);
        if ((std::abs(y) - z * tanV) * cosV > radius) return false;
        return true;
    }
};

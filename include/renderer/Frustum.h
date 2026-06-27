#pragma once

#include "WorldCoord.h"
#include "renderer/CameraBasis.h"

#include <cmath>
#include <glm/glm.hpp>

// Conservative view-frustum test for chunk bounding spheres. Plane-distance
// tests against the bounding sphere err on the side of "visible" — this never
// culls geometry the renderer would actually draw, it only skips submitting
// chunks behind the camera or outside the view cone.
struct Frustum {
    glm::dvec3 pos{}, fwd{}, right{}, up{};
    double tanH = 0.0, tanV = 0.0, cosH = 1.0, cosV = 1.0, farClip = 0.0;

    // worldUp / roll thread the surface-normal camera orientation (M17) through
    // culling so the frustum planes rotate with the view: default +Y up and roll 0
    // reproduce the historical Y-up frame bit-for-bit (cameraBasis short-circuits
    // the rotation to the identity), so culling decisions are unchanged.
    void update(const WorldCoord& camPos, float pitch, float yaw,
                double aspect, double vfovDeg, double farClipM,
                const glm::dvec3& worldUp = glm::dvec3(0.0, 1.0, 0.0),
                double roll = 0.0) {
        pos = camPos.value;
        const CameraBasis b = cameraBasis(pitch, yaw, roll, worldUp);
        fwd   = b.forward;
        right = b.right;
        up    = b.up;
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

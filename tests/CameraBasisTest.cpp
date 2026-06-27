// Tests for the surface-normal camera basis (include/renderer/CameraBasis.h, M17).
//
// cameraBasis() builds an orthonormal forward/right/up frame from pitch/yaw/roll
// interpreted in a frame whose "up" is an arbitrary world-up vector. The default
// (+Y up, roll 0) must reproduce the historical Y-up formulas byte-for-byte; an
// arbitrary up must give a level horizon (forward ⊥ up at pitch 0) and a frame
// aligned to that up — the renderer change that lets a player on the +X face of a
// body see a level horizon.

#include "renderer/CameraBasis.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

void expectVecNear(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-9) {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

void expectOrthonormal(const CameraBasis& b) {
    EXPECT_NEAR(glm::length(b.forward), 1.0, 1e-9);
    EXPECT_NEAR(glm::length(b.right),   1.0, 1e-9);
    EXPECT_NEAR(glm::length(b.up),      1.0, 1e-9);
    EXPECT_NEAR(glm::dot(b.forward, b.right), 0.0, 1e-9);
    EXPECT_NEAR(glm::dot(b.forward, b.up),    0.0, 1e-9);
    EXPECT_NEAR(glm::dot(b.right,   b.up),    0.0, 1e-9);
    // up == forward × right (the renderer's convention; see render() / Frustum).
    expectVecNear(b.up, glm::cross(b.forward, b.right));
}

}  // namespace

// The default up reproduces the exact historical Y-up formulas, for several
// pitch/yaw — the byte-identical-default guarantee the renderer relies on.
TEST(CameraBasis, DefaultUpMatchesHistoricalYUp) {
    const double pitches[] = {0.0, 0.3, -0.7, 1.2};
    const double yaws[]    = {0.0, 0.5, 2.1, -1.3};
    for (double pitch : pitches) {
        for (double yaw : yaws) {
            const double cp = std::cos(pitch), sp = std::sin(pitch);
            const double cy = std::cos(yaw),   sy = std::sin(yaw);
            const glm::dvec3 fwd(cp * sy, sp, cp * cy);
            const glm::dvec3 right(cy, 0.0, -sy);
            const glm::dvec3 up = glm::cross(fwd, right);

            const CameraBasis b = cameraBasis(pitch, yaw, 0.0);
            // Bit-identical: the rotation short-circuits to the identity for +Y.
            EXPECT_EQ(b.forward.x, fwd.x);
            EXPECT_EQ(b.forward.y, fwd.y);
            EXPECT_EQ(b.forward.z, fwd.z);
            EXPECT_EQ(b.right.x, right.x);
            EXPECT_EQ(b.right.y, right.y);
            EXPECT_EQ(b.right.z, right.z);
            EXPECT_EQ(b.up.x, up.x);
            EXPECT_EQ(b.up.y, up.y);
            EXPECT_EQ(b.up.z, up.z);
        }
    }
}

// Standing on the +X face of a body: up aligns with +X, the camera still looks
// along its yaw=0 forward (+Z), and the horizon is level (forward ⊥ up).
TEST(CameraBasis, PlusXUpGivesLevelHorizon) {
    const CameraBasis b = cameraBasis(0.0, 0.0, 0.0, glm::dvec3(1.0, 0.0, 0.0));
    expectOrthonormal(b);
    expectVecNear(b.up, glm::dvec3(1.0, 0.0, 0.0));
    expectVecNear(b.forward, glm::dvec3(0.0, 0.0, 1.0));
    EXPECT_NEAR(glm::dot(b.forward, b.up), 0.0, 1e-9);  // level horizon
}

// At pitch 0 the forward lies in the surface tangent plane (⊥ the up vector) for
// any yaw and any up — i.e. "look level" stays level on an arbitrary surface.
TEST(CameraBasis, PitchZeroForwardIsPerpendicularToUp) {
    const glm::dvec3 ups[] = {
        glm::normalize(glm::dvec3(1.0, 0.0, 0.0)),
        glm::normalize(glm::dvec3(1.0, 1.0, 0.0)),
        glm::normalize(glm::dvec3(-2.0, 1.0, 3.0)),
        glm::normalize(glm::dvec3(0.0, 0.0, 1.0)),
    };
    for (const glm::dvec3& up : ups) {
        for (double yaw : {0.0, 0.8, 2.5, -1.1}) {
            const CameraBasis b = cameraBasis(0.0, yaw, 0.0, up);
            expectOrthonormal(b);
            EXPECT_NEAR(glm::dot(b.forward, up), 0.0, 1e-9)
                << "forward should be tangent to the surface at pitch 0";
            // The camera up sits on the same side as the surface up.
            EXPECT_GT(glm::dot(b.up, up), 0.0);
        }
    }
}

// The frame stays orthonormal for arbitrary pitch/yaw/roll and up.
TEST(CameraBasis, OrthonormalForArbitraryInputs) {
    const glm::dvec3 up = glm::normalize(glm::dvec3(2.0, -1.0, 0.5));
    for (double pitch : {-1.0, 0.0, 0.9}) {
        for (double yaw : {0.0, 1.7, -2.2}) {
            for (double roll : {0.0, 0.6, -1.4}) {
                expectOrthonormal(cameraBasis(pitch, yaw, roll, up));
            }
        }
    }
}

// An upside-down (antipodal) up flips the frame cleanly rather than degenerating.
TEST(CameraBasis, AntipodalUpFlipsFrame) {
    const CameraBasis b = cameraBasis(0.0, 0.0, 0.0, glm::dvec3(0.0, -1.0, 0.0));
    expectOrthonormal(b);
    expectVecNear(b.up, glm::dvec3(0.0, -1.0, 0.0));
    expectVecNear(b.forward, glm::dvec3(0.0, 0.0, -1.0));
}

// Roll about the forward axis tilts up/right but leaves the look direction fixed.
TEST(CameraBasis, RollRotatesAboutForward) {
    const CameraBasis b0 = cameraBasis(0.0, 0.0, 0.0);
    const CameraBasis br = cameraBasis(0.0, 0.0, 0.5);
    expectVecNear(br.forward, b0.forward);   // forward unchanged by roll
    EXPECT_GT(glm::length(br.up - b0.up), 1e-3);  // up actually moved
    expectOrthonormal(br);
}

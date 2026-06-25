#include "renderer/Frustum.h"

#include <gtest/gtest.h>

namespace {

Frustum makeFrustum(const WorldCoord& pos, float pitch, float yaw, double farClip = 1000.0) {
    Frustum f;
    f.update(pos, pitch, yaw, 16.0 / 9.0, 60.0, farClip);
    return f;
}

TEST(Frustum, ChunkDirectlyAheadIsVisible) {
    Frustum f = makeFrustum(WorldCoord(0, 0, 0), 0.0f, 0.0f, 1000.0);
    // Camera looks along +Z (yaw=0). A chunk 50 m ahead should be visible.
    EXPECT_TRUE(f.sphereVisible(glm::dvec3(0, 0, 50), 10.0));
}

TEST(Frustum, ChunkBehindCameraIsHidden) {
    Frustum f = makeFrustum(WorldCoord(0, 0, 0), 0.0f, 0.0f, 1000.0);
    // A chunk 50 m behind the camera (negative Z) should be culled.
    EXPECT_FALSE(f.sphereVisible(glm::dvec3(0, 0, -50), 10.0));
}

TEST(Frustum, ChunkBeyondFarClipIsHidden) {
    Frustum f = makeFrustum(WorldCoord(0, 0, 0), 0.0f, 0.0f, 100.0);
    EXPECT_FALSE(f.sphereVisible(glm::dvec3(0, 0, 200), 10.0));
}

TEST(Frustum, ChunkFarToTheSideIsHidden) {
    Frustum f = makeFrustum(WorldCoord(0, 0, 0), 0.0f, 0.0f, 1000.0);
    // A chunk 200 m to the right, only 10 m ahead — well outside the 60° FOV.
    EXPECT_FALSE(f.sphereVisible(glm::dvec3(200, 0, 10), 5.0));
}

TEST(Frustum, ChunkNearBorderlineIsConservativelyVisible) {
    Frustum f = makeFrustum(WorldCoord(0, 0, 0), 0.0f, 0.0f, 1000.0);
    // A large sphere that overlaps the near plane should still be visible.
    EXPECT_TRUE(f.sphereVisible(glm::dvec3(0, 0, -5), 20.0));
}

TEST(Frustum, ChunkAboveCameraIsHidden) {
    Frustum f = makeFrustum(WorldCoord(0, 0, 0), 0.0f, 0.0f, 1000.0);
    // A small chunk 200 m above, only 10 m ahead — outside vertical FOV.
    EXPECT_FALSE(f.sphereVisible(glm::dvec3(0, 200, 10), 5.0));
}

TEST(Frustum, RotatedCameraSeesChunkInNewDirection) {
    // Yaw = π/2 → camera looks along +X.
    float yaw = static_cast<float>(3.14159265358979323846 / 2.0);
    Frustum f = makeFrustum(WorldCoord(0, 0, 0), 0.0f, yaw, 1000.0);
    EXPECT_TRUE(f.sphereVisible(glm::dvec3(50, 0, 0), 10.0));
    EXPECT_FALSE(f.sphereVisible(glm::dvec3(0, 0, 50), 5.0));
}

}  // namespace

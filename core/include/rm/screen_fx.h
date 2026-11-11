#pragma once

// Drawing over the game where its world is (2026-11-11, the user: "couldn't we make our own ray's
// arc"): the game's camera maths (research/re5-engine-map.md "The camera"), right-handed with row
// vectors as its own (uCameraBase's view 0x00835C20 -> 0x006D1030, projection 0x00835C50 ->
// 0x006D1650), and the shape of a lightning bolt between two points on the screen.

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rm::screenfx {

using Mat4 = std::array<float, 16>;  // row major; a point is a row: [x y z 1] * M

/// The game's view matrix: eye, target and up as its cameras keep them.
Mat4 LookAtRH(const float eye[3], const float target[3], const float up[3]);
/// The game's projection: the vertical field of view in degrees (as the camera keeps it), the aspect.
Mat4 PerspectiveFovRH(float fovDegrees, float aspect, float nearPlane, float farPlane);
Mat4 Multiply(const Mat4& a, const Mat4& b);

struct ScreenPoint {
    float x = 0.0f, y = 0.0f;  // pixels, from the top left
    float depth = 0.0f;        // the distance ahead of the camera
};
/// Where `p` shows on a screen of `width` x `height` through `viewProj` (view * projection);
/// nullopt behind the camera (or too close to it) or far outside the screen.
std::optional<ScreenPoint> Project(const Mat4& viewProj, const float p[3], float width, float height);

/// A lightning bolt from (ax, ay) to (bx, by): `segments` + 1 points (the first and last exactly
/// the ends), the ones between pushed sideways by up to `jitter` of the length, the middle most
/// (midpoint displacement); the same `seed` gives the same bolt.
std::vector<std::pair<float, float>> BoltPath(float ax, float ay, float bx, float by, int segments, float jitter, std::uint32_t seed);

}  // namespace rm::screenfx

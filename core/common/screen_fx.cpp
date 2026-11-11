#include "rm/screen_fx.h"

#include <cmath>

namespace rm::screenfx {
namespace {

void Normalise(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length < 1.1920929e-07f) return;  // the game's own test (0x006D1030)
    for (int i = 0; i < 3; ++i) v[i] /= length;
}

void Cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A small deterministic generator for the bolt's offsets.
std::uint32_t Next(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

}  // namespace

Mat4 LookAtRH(const float eye[3], const float target[3], const float up[3]) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    Normalise(f);
    float s[3];
    Cross(up, f, s);  // as the game: cross(up, forward), then the matrix's first column negated
    Normalise(s);
    float u[3];
    Cross(f, s, u);
    Mat4 m{};
    m[0] = -s[0];
    m[4] = -s[1];
    m[8] = -s[2];
    m[12] = Dot(eye, s);
    m[1] = u[0];
    m[5] = u[1];
    m[9] = u[2];
    m[13] = -Dot(eye, u);
    m[2] = -f[0];
    m[6] = -f[1];
    m[10] = -f[2];
    m[14] = Dot(eye, f);
    m[15] = 1.0f;
    return m;
}

Mat4 PerspectiveFovRH(float fovDegrees, float aspect, float nearPlane, float farPlane) {
    Mat4 m{};
    const float yScale = 1.0f / std::tan(fovDegrees * 0.017453292f * 0.5f);
    const float q = farPlane / (farPlane - nearPlane);
    m[0] = yScale / aspect;
    m[5] = yScale;
    m[10] = -q;
    m[11] = -1.0f;
    m[14] = -q * nearPlane;
    return m;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += a[static_cast<std::size_t>(r * 4 + k)] * b[static_cast<std::size_t>(k * 4 + c)];
            out[static_cast<std::size_t>(r * 4 + c)] = v;
        }
    }
    return out;
}

std::optional<ScreenPoint> Project(const Mat4& m, const float p[3], float width, float height) {
    const float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    const float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    const float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
    if (w < 1.0f) return std::nullopt;  // behind the camera, or within a unit (1 cm) of it
    const float nx = x / w, ny = y / w;
    if (std::fabs(nx) > 4.0f || std::fabs(ny) > 4.0f) return std::nullopt;  // far outside the screen
    ScreenPoint out;
    out.x = (nx * 0.5f + 0.5f) * width;
    out.y = (0.5f - ny * 0.5f) * height;
    out.depth = w;
    return out;
}

std::vector<std::pair<float, float>> BoltPath(float ax, float ay, float bx, float by, int segments, float jitter, std::uint32_t seed) {
    if (segments < 1) segments = 1;
    const auto count = static_cast<std::size_t>(segments) + 1;
    std::vector<std::pair<float, float>> points(count);
    const float dx = bx - ax, dy = by - ay;
    const float length = std::sqrt(dx * dx + dy * dy);
    const float px = length > 0.0f ? -dy / length : 0.0f, py = length > 0.0f ? dx / length : 0.0f;  // sideways
    std::uint32_t state = seed == 0 ? 0x9E3779B9u : seed;
    for (std::size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        float offset = 0.0f;
        if (i != 0 && i + 1 != count) {
            // A random push, largest mid-way (4t(1 - t)), up to jitter * length either side.
            const float r = static_cast<float>(Next(state) % 2001u) / 1000.0f - 1.0f;
            offset = r * jitter * length * 4.0f * t * (1.0f - t);
        }
        points[i] = {ax + dx * t + px * offset, ay + dy * t + py * offset};
    }
    return points;
}

}  // namespace rm::screenfx

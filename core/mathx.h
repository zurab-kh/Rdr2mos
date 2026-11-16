#pragma once
#include <cmath>
#include <cstdint>
#include "bytes.h"

namespace coop {

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline float distance(Vec3 a, Vec3 b) { return length(a - b); }
inline float distanceSq(Vec3 a, Vec3 b) {
    Vec3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

// Shortest signed difference between two headings in degrees, in (-180, 180].
inline float angleDelta(float from, float to) {
    float d = std::fmod(to - from, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d <= -180.0f) d += 360.0f;
    return d;
}
inline float wrapHeading(float h) {
    h = std::fmod(h, 360.0f);
    return h < 0 ? h + 360.0f : h;
}
inline float lerpHeading(float a, float b, float t) { return wrapHeading(a + angleDelta(a, b) * t); }

// Jenkins one-at-a-time hash, the same one the game uses for model/weapon names.
constexpr uint32_t joaat(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        h += static_cast<uint8_t>(c);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

}  // namespace coop

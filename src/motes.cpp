#include "motes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
const float kPi = 3.14159265f;

// Steady per-mote randomness in [0, 1): mote k, channel c. Each mote keeps its own size,
// flicker and offset from frame to frame.
float noise(int k, int c) {
    uint32_t x = static_cast<uint32_t>(k) * 0x9E3779B1u ^ static_cast<uint32_t>(c + 1) * 0x85EBCA77u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    x *= 0x297A2D39u;
    x ^= x >> 15;
    return (x & 0xFFFFFF) / 16777216.0f;
}
}  // namespace

float trailFade(float d, float strength) {
    float a = d < 30 ? 1 : std::max(0.0f, 1 - (d - 30) / 60);
    return a * std::clamp((d - 1.2f) / 1.8f, 0.0f, 1.0f) * strength;
}

RouteSampler::RouteSampler(const std::vector<Vec3>& trail, float trailBase) : trail_(trail) {
    if (trail.size() < 2) return;
    at_.resize(trail.size());
    at_[0] = trailBase - length(trail[1] - trail[0]);
    for (size_t i = 1; i < trail.size(); ++i)
        at_[i] = i == 1 ? trailBase : at_[i - 1] + length(trail[i] - trail[i - 1]);
}

bool RouteSampler::sample(float m, Vec3& pos, Vec3& dir) {
    if (at_.empty() || m < at_.front() || m > at_.back()) return false;
    while (seg_ > 0 && at_[seg_] > m) --seg_;
    while (seg_ + 2 < at_.size() && at_[seg_ + 1] < m) ++seg_;
    float len = at_[seg_ + 1] - at_[seg_];
    Vec3 d = trail_[seg_ + 1] - trail_[seg_];
    pos = trail_[seg_] + d * (len > 0 ? (m - at_[seg_]) / len : 0);
    float dl = length(d);
    dir = dl > 0 ? d * (1 / dl) : Vec3{0, 0, 1};
    return true;
}

float moteRadius(float focal, float depth, float size, float s) {
    return std::clamp(0.06f * focal / depth * size, 1.6f * s, 6.0f * s);
}

void buildMotes(const std::vector<Vec3>& trail, float trailBase, float t, float strength, int style,
                std::vector<Mote>& out) {
    out.clear();
    if (trail.size() < 2) return;
    const float pi2 = 2 * kPi;

    RouteSampler route(trail, trailBase);
    const float feet = route.feet(), end = route.end();

    auto add = [&](float m, Vec3 offset, float bright, float size, float tailLen) {
        Vec3 pos, dir;
        float fade = trailFade(m - feet, strength);
        if (fade * bright < 0.02f || !route.sample(m, pos, dir)) return;
        Vec3 side{dir.z, 0, -dir.x};  // across the path, level
        float sl = length(side);
        side = sl > 0 ? side * (1 / sl) : Vec3{1, 0, 0};
        Vec3 p = pos + side * offset.x + Vec3{0, offset.y, 0} + dir * offset.z;
        out.push_back({p, bright * fade, size, dir * -tailLen + Vec3{0, -0.04f * tailLen, 0}, tailLen > 0});
    };
    // Motes k = first..last at k * spacing + flow metres along the route.
    auto each = [&](float spacing, float flow, auto&& fn) {
        int first = static_cast<int>(std::ceil((feet - flow) / spacing));
        int last = static_cast<int>(std::floor((end - flow) / spacing));
        for (int k = first; k <= last && k - first < 600; ++k) fn(k, k * spacing + flow);
    };

    switch (style) {
        case kMoteEmbers:
            // Embers drifting forward: scattered around the path, each its own size and flicker,
            // with a short glowing tail.
            each(1.3f, t * 1.0f, [&](int k, float m) {
                float flick = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(t * (2.5f + 2 * noise(k, 3)) + pi2 * noise(k, 4)));
                Vec3 off{(noise(k, 1) - 0.5f) * 0.5f, 0.15f + noise(k, 2) * 0.35f + 0.06f * std::sin(t * 1.3f + k), 0};
                add(m, off, flick, 0.6f + 0.8f * noise(k, 5), 0.45f);
            });
            break;
        case kMotePulse:
            // Still motes with soft bands of light travelling through them towards the destination.
            each(0.9f, 0, [&](int, float m) {
                float wave = 0.5f + 0.5f * std::cos(pi2 * (m - t * 5.5f) / 7.0f);
                float crest = wave * wave * wave;
                add(m, Vec3{0, 0.25f, 0}, 0.5f + 0.5f * crest, 1.0f + 0.6f * crest, 0);
            });
            break;
        case kMoteWisps:
            // Two strands of tiny motes spiralling around the path as they flow along it.
            each(0.55f, t * 1.3f, [&](int k, float m) {
                float a = m * 2.2f + (k & 1) * kPi + t * 0.5f;
                Vec3 off{std::cos(a) * 0.22f, 0.35f + std::sin(a) * 0.22f, 0};
                add(m, off, 0.7f + 0.3f * std::sin(a), 0.55f, 0);
            });
            break;
        case kMoteStream:
            // Sparks rising off the path painted on the ground (the in-game renderer draws the
            // path itself): each drifts up and forward, flickering, and burns out.
            each(2.0f, 0, [&](int k, float m) {
                float life = 1.6f + 1.2f * noise(k, 2);
                float u = std::fmod(t / life + noise(k, 3), 1.0f);
                float flick = 0.7f + 0.3f * std::sin(t * (9 + 6 * noise(k, 4)) + pi2 * noise(k, 5));
                float swirl = pi2 * noise(k, 6) + t * 1.7f;
                Vec3 off{(noise(k, 1) - 0.5f) * 0.45f + 0.06f * std::sin(swirl), 0.05f + u * (0.6f + 0.4f * noise(k, 7)),
                         u * 0.7f + 0.08f * std::cos(swirl)};
                float glow = std::sin(kPi * u);
                add(m, off, glow * std::sqrt(glow) * flick * 0.6f, 0.25f + 0.2f * noise(k, 8), 0);
            });
            break;
        default:  // kMoteFireflies
            // Sparse lights that fade in, wander gently, drift forward and fade out again.
            each(2.2f, t * 0.5f, [&](int k, float m) {
                float period = 3 + 2 * noise(k, 2);
                float u = std::fmod(t / period + noise(k, 3), 1.0f);
                float life = std::sin(kPi * u);
                Vec3 off{0.5f * std::sin(t * 0.6f * (1 + noise(k, 4)) + pi2 * noise(k, 5)),
                         0.3f + 0.35f * (0.5f + 0.5f * std::sin(t * 0.8f + k)), noise(k, 1) - 0.5f};
                add(m, off, life * life, 0.8f + 0.6f * noise(k, 6), 0);
            });
            break;
    }
}

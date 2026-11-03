#include "nav.h"

#include <algorithm>
#include <cmath>

namespace {
const float kPi = 3.14159265f;
constexpr size_t kNeeded = 50;     // movement samples before trusting the fit
constexpr size_t kWindow = 120;    // keep only the most recent samples
constexpr uint64_t kSampleMs = 200;
}  // namespace

float horizontalDist(const Vec3& a, const Vec3& b) {
    return std::hypot(b.x - a.x, b.z - a.z);
}

float bearing(const Vec3& a, const Vec3& b) { return std::atan2(b.x - a.x, b.z - a.z); }

float wrapAngle(float a) {
    a = std::fmod(a + kPi, 2 * kPi);
    if (a < 0) a += 2 * kPi;
    return a - kPi;
}

void HeadingCalibrator::reset() {
    samples_.clear();
    haveLast_ = false;
    done_ = false;
}

void HeadingCalibrator::set(int sign, float offset) {
    sign_ = sign;
    offset_ = offset;
    done_ = true;
}

int HeadingCalibrator::percent() const {
    return done_ ? 100 : static_cast<int>(std::min<size_t>(samples_.size() * 100 / kNeeded, 99));
}

void HeadingCalibrator::feed(const PlayerState& p, uint64_t nowMs) {
    if (done_ || !p.valid) return;
    if (!haveLast_) {
        last_ = p.pos;
        lastMs_ = nowMs;
        haveLast_ = true;
        return;
    }
    if (nowMs - lastMs_ < kSampleMs) return;

    float dist = horizontalDist(last_, p.pos);
    float speed = dist / ((nowMs - lastMs_) / 1000.0f);
    // Running/sprinting only; skips idling, slow strafing and warps/loading screens.
    if (speed > 2.5f && speed < 15.0f) {
        samples_.push_back({bearing(last_, p.pos), p.heading});
        if (samples_.size() > kWindow) samples_.erase(samples_.begin());
    }
    last_ = p.pos;
    lastMs_ = nowMs;

    if (samples_.size() < kNeeded) return;
    // Fit facing = s*heading + o for s = +1 and -1 and keep the tighter one.
    float bestR = 0, bestO = 0;
    int bestS = 1;
    for (int s : {1, -1}) {
        float c = 0, sn = 0;
        for (auto& smp : samples_) {
            float d = smp.moveBearing - s * smp.heading;
            c += std::cos(d);
            sn += std::sin(d);
        }
        float r = std::hypot(c, sn) / samples_.size();
        if (r > bestR) {
            bestR = r;
            bestS = s;
            bestO = std::atan2(sn, c);
        }
    }
    // Lock-on strafing and rolling add noise; only accept a clearly consistent fit.
    if (bestR > 0.85f) set(bestS, bestO);
}

void RouteFollower::update(const Step& step, const Vec3& pos, float radius) {
    const auto& wps = step.waypoints;
    if (wps.empty()) {
        index_ = 0;
        return;
    }
    // Height counts more than ground distance: on stairs, in towers and in Blighttown the point
    // straight above or below you is on another floor, not nearby.
    auto apart = [&](const Vec3& w) {
        float dy = (pos.y - w.y) * 3;
        return std::sqrt(horizontalDist(pos, w) * horizontalDist(pos, w) + dy * dy);
    };
    if (!synced_) {
        // Entering a step mid-route: aim for the closest waypoint rather than the first one.
        size_t best = 0;
        for (size_t i = 1; i < wps.size(); ++i)
            if (apart(wps[i]) < apart(wps[best])) best = i;
        index_ = best;
        synced_ = true;
    }
    // Any waypoint at or past the current one counts, so shortcuts don't strand the arrow. It has
    // to be on your floor (a storey is about 4 m), or a spiral stair would skip a whole turn.
    for (size_t j = wps.size(); j-- > index_;) {
        bool reached = horizontalDist(pos, wps[j]) <= radius && std::fabs(pos.y - wps[j].y) <= 2.5f;
        if (reached) {
            index_ = j + 1;
            break;
        }
    }
}

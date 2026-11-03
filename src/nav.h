#pragma once
#include <cstdint>
#include <vector>

#include "game.h"
#include "guide.h"

float horizontalDist(const Vec3& a, const Vec3& b);
// World bearing from a to b in the x/z plane, radians, atan2(dx, dz).
float bearing(const Vec3& a, const Vec3& b);
float wrapAngle(float a);  // to [-pi, pi]

// Maps the game's facing angle onto world bearings: facing = sign * heading + offset.
// The convention isn't documented, so it's learned by watching the player run: a running
// character faces the way it moves.
class HeadingCalibrator {
public:
    void feed(const PlayerState& p, uint64_t nowMs);
    void reset();
    bool done() const { return done_; }
    int percent() const;
    int sign() const { return sign_; }
    float offset() const { return offset_; }
    void set(int sign, float offset);

private:
    struct Sample {
        float moveBearing, heading;
    };
    std::vector<Sample> samples_;
    Vec3 last_{};
    uint64_t lastMs_ = 0;
    bool haveLast_ = false;
    bool done_ = false;
    int sign_ = 1;
    float offset_ = 0;
};

// Tracks which waypoint of a step the player is heading for.
class RouteFollower {
public:
    void reset() { synced_ = false; index_ = 0; }
    void update(const Step& step, const Vec3& pos, float radius);
    size_t index() const { return index_; }  // == waypoints.size() once arrived
    bool arrived(const Step& step) const { return index_ >= step.waypoints.size(); }

private:
    size_t index_ = 0;
    bool synced_ = false;
};

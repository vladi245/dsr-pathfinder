#pragma once
#include <vector>

#include "camera.h"

// How the trail looks (config trail_style). Stream needs the in-game renderer: it paints the
// path onto the ground itself; the overlay window shows it as pulse.
enum MoteStyle { kMoteEmbers, kMotePulse, kMoteWisps, kMoteFireflies, kMoteStream };

// One light of the trail: where it is, how bright and big, and an optional tail (a world offset
// behind it). Shared by the overlay window and the in-game renderer, so both draw the same trail.
struct Mote {
    Vec3 pos;
    float bright, size;
    Vec3 tail;
    bool hasTail;
};

// Points along the trail (from your feet to the destination) by distance along the step's route,
// so anything placed with it stays pinned to the route instead of sliding along with you.
// trailBase is how far along the step's route trail[1] is.
class RouteSampler {
public:
    RouteSampler(const std::vector<Vec3>& trail, float trailBase);
    float feet() const { return at_.empty() ? 0 : at_.front(); }  // route distance at your feet
    float end() const { return at_.empty() ? 0 : at_.back(); }    // ...and at the destination
    // Position and unit heading `m` metres along the route; false outside feet()..end().
    bool sample(float m, Vec3& pos, Vec3& dir);

private:
    const std::vector<Vec3>& trail_;
    std::vector<float> at_;
    size_t seg_ = 0;
};

// The motes along `trail` at time `t` seconds. strength: 1 = full, lower while calm.
void buildMotes(const std::vector<Vec3>& trail, float trailBase, float t, float strength, int style,
                std::vector<Mote>& out);

// How strongly the trail shows `d` metres from your feet: faded in at your feet, clear for the
// next 30 m, then fading out.
float trailFade(float d, float strength);

// Mote radius in pixels: `focal` is pixels per metre at 1 m depth, `s` the UI scale.
float moteRadius(float focal, float depth, float size, float s);

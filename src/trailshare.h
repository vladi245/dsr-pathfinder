#pragma once
// The block of shared memory between the app and its in-game renderer (src/hook). The app
// writes the trail and where the camera lives; the renderer, inside the game, reads the camera
// itself every frame and draws the motes into the game's own image. Header-only, used by both.
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace trailshare {

// Named by version, so an older renderer still loaded in the game never reads a newer layout.
constexpr wchar_t kMappingName[] = L"Local\\DSRPathfinder.Trail.4";
constexpr uint32_t kMagic = 0x54525344;  // "DSRT"
constexpr uint32_t kVersion = 4;
constexpr uint32_t kMaxPoints = 4096;
// The renderer draws only while the app wrote within this long, so quitting the app (or it
// hanging) takes the trail away.
constexpr uint64_t kAppTimeoutMs = 500;
// The app leaves the motes to the renderer while the renderer answered within this long.
constexpr uint64_t kHookTimeoutMs = 1000;  // game hitches stay in game, not a flash of the overlay's

// What the app sends each tick.
struct Frame {
    uint32_t draw;            // 1 = show the motes now
    uint32_t style;           // MoteStyle
    uint32_t occlusion;       // 1 = dim motes behind walls, using the game's depth buffer
    uint32_t count;           // trail points
    uint32_t endsAtGoal;      // 1 = the last point is the destination (gets a ring on the ground)
    uint64_t camAddr;         // the camera matrix in the game, see CameraLocator::Source
    uint64_t projAddr;        // its projection matrix, 0 = use xs/ys
    int32_t camKind, camSign;
    float xs, ys;             // projection scales when projAddr is 0
    float trailBase, strength;
    // The projection matrix as the app last read it valid. The game reuses its slot during a
    // frame, so at Present it can hold something else; this stands in then.
    uint32_t projValid;
    float proj[16];
    float points[kMaxPoints][3];
};

struct Block {
    uint32_t magic, version;
    volatile LONG seq;            // odd while the app is writing `frame`
    uint32_t pad0;
    volatile uint64_t appMs;      // GetTickCount64 of the app's last write
    Frame frame;
    // Written by the renderer.
    volatile uint64_t hookMs;     // GetTickCount64 of its last frame with a readable camera
    volatile uint32_t hookVersion;
    volatile LONG owner;          // the renderer that draws: the latest one loaded
    char hookInfo[320];           // status lines for the F11 debug view, one per line
};

// App side: publish a frame (single writer).
inline void write(Block& b, const Frame& f, uint32_t count, uint64_t nowMs) {
    InterlockedIncrement(&b.seq);  // odd: writing
    MemoryBarrier();
    std::memcpy(&b.frame, &f, offsetof(Frame, points) + count * sizeof f.points[0]);
    b.frame.count = count;
    MemoryBarrier();
    InterlockedIncrement(&b.seq);  // even: done
    b.appMs = nowMs;
}

// Renderer side: copy a consistent frame. False while the app is mid-write.
inline bool read(const Block& b, Frame& out) {
    LONG s0 = b.seq;
    MemoryBarrier();
    if (s0 & 1) return false;
    uint32_t count = b.frame.count;
    if (count > kMaxPoints) return false;
    std::memcpy(&out, const_cast<const Frame*>(&b.frame), offsetof(Frame, points) + count * sizeof out.points[0]);
    MemoryBarrier();
    return b.seq == s0 && out.count == count;
}

}  // namespace trailshare

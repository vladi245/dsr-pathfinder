#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "game.h"

Vec3 operator+(const Vec3& a, const Vec3& b);
Vec3 operator-(const Vec3& a, const Vec3& b);
Vec3 operator*(const Vec3& a, float s);
float dot(const Vec3& a, const Vec3& b);
float length(const Vec3& a);

// The game camera, enough to project world points onto the screen.
struct Camera {
    Vec3 eye;
    Vec3 right, up, fwd;  // world-space axes; fwd points into the screen
    float xs = 1, ys = 1;  // projection scale: ndc = (xs * x / depth, ys * y / depth)
    bool fovFound = false;  // ys came from the game's projection matrix, not the fallback

    Vec3 toView(const Vec3& p) const {
        Vec3 d = p - eye;
        return {dot(d, right), dot(d, up), dot(d, fwd)};
    }
    float aspect() const { return ys / xs; }
};

// The camera stored in the 16 floats at `f`, read the way the locator found it (kind and depth
// sign, see CameraLocator::Candidate). False if it isn't a valid camera right now.
bool cameraFromMatrix(const float* f, uint8_t kind, int sign, Camera& out);
// The x and y projection scales of a perspective matrix, if `f` is one.
bool projectionScale(const float* f, float& xs, float& ys);

// Finds the camera's matrix in game memory without any fixed address. A background thread scans
// the game's writable memory for 4x4 rigid transforms whose eye sits a few metres from the player
// and looks at them. Candidates that keep doing that while the camera turns are the camera.
// Read-only, like everything else here.
class CameraLocator {
public:
    ~CameraLocator();
    void reset();  // the game closed: forget everything
    // Call every tick. aspect = game window width / height. fallbackFov in radians (vertical);
    // forceFov makes it override the projection read from the game.
    void update(const Process& proc, const PlayerState& ps, float aspect, float fallbackFov,
                bool forceFov, uint64_t nowMs);
    const std::optional<Camera>& camera() const { return cam_; }
    bool locked() const { return locked_ != 0; }
    std::wstring status() const;
    // Where the locked camera and its projection live in the game, for the in-game renderer,
    // which reads them itself every frame. addr 0 = none.
    struct Source {
        uintptr_t camAddr = 0, projAddr = 0;
        uint8_t kind = 0;
        int sign = 1;
        const float* proj = nullptr;  // the last valid projection matrix read, or null
    };
    Source source() const {
        bool fov = cam_ && cam_->fovFound;
        return {locked_, fov ? proj_.addr : 0, lockedKind_, lockedSign_, fov && projValid_ ? projM_ : nullptr};
    }

    // A candidate matrix: where it is and how to read it.
    struct Candidate {
        uintptr_t addr = 0;
        uint8_t kind = 0;  // bit 0: translation in the 4th column; bit 1: world-to-camera
        int sign = 1;      // +1 if depth runs along +z, -1 along -z
    };
    struct Projection {
        uintptr_t addr = 0;
        float xs = 0, ys = 0;
        float m[16] = {};  // the whole matrix as the scan found it
    };
    // The scan itself, exposed for the offline test. Blocks; returns false if cancelled.
    static bool scan(DWORD pid, const Vec3& player, const std::atomic<bool>& cancel,
                     std::vector<Candidate>& cams, std::vector<Projection>& projs,
                     size_t& bytesScanned);

private:
    struct Tracked {
        Candidate c;
        float lastYaw = 0, turned = 0;
        uint64_t since = 0;
    };
    struct ScanJob {
        std::atomic<bool> cancel{false}, done{false};
        std::vector<Candidate> cams;
        std::vector<Projection> projs;
        size_t bytes = 0;
        std::thread thread;
    };

    void startScan(DWORD pid, const Vec3& player, uint64_t nowMs);
    void finishScan(float aspect, uint64_t nowMs);
    void stopScan();

    std::unique_ptr<ScanJob> job_;
    std::vector<Tracked> tracked_;
    uintptr_t locked_ = 0, lastLocked_ = 0;
    uint8_t lockedKind_ = 0;
    int lockedSign_ = 1;
    uint64_t lastGood_ = 0, nextScan_ = 0, scanStarted_ = 0;
    int scans_ = 0;
    size_t lastScanBytes_ = 0;
    uint64_t lastScanMs_ = 0;
    Projection proj_;
    float projM_[16] = {};  // its whole matrix when last valid (the slot is reused within a frame)
    bool projValid_ = false;
    std::vector<std::pair<float, int>> candidates_;  // fitting projections: fov, copies (for F11)
    std::optional<Camera> cam_;
};

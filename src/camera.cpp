#include "camera.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }

namespace {
const float kPi = 3.14159265f;
const float kScanAngle = 30 * kPi / 180;  // the player must be this close to the view centre
constexpr uint64_t kRescanMs = 45000;      // refresh candidates while waiting for a camera turn
constexpr uint64_t kRetryMs = 5000;        // scan again this soon when nothing turned up
constexpr uint64_t kLockTurnMs = 1000;
const float kLockTurn = 1.0f;              // radians of camera yaw a candidate must follow
constexpr size_t kMaxCandidates = 512;

float wrap(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

bool near0(float v, float tol) { return std::fabs(v) <= tol; }  // false for NaN

// A rigid transform read one of four ways: eye position and camera axes in world space.
struct Pose {
    Vec3 eye, ax[3];
};

bool decode(const float* f, uint8_t kind, Pose& p) {
    float m[4][4];  // row-vector convention: p' = p * m, translation in row 3
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r][c] = (kind & 1) ? f[c * 4 + r] : f[r * 4 + c];
    if (!near0(m[0][3], 1e-4f) || !near0(m[1][3], 1e-4f) || !near0(m[2][3], 1e-4f) ||
        !near0(m[3][3] - 1, 1e-4f))
        return false;
    Vec3 row[3];
    for (int r = 0; r < 3; ++r) {
        row[r] = {m[r][0], m[r][1], m[r][2]};
        if (!near0(length(row[r]) - 1, 2e-3f)) return false;
    }
    if (!near0(dot(row[0], row[1]), 2e-3f) || !near0(dot(row[0], row[2]), 2e-3f) ||
        !near0(dot(row[1], row[2]), 2e-3f))
        return false;
    Vec3 t{m[3][0], m[3][1], m[3][2]};
    if (!(length(t) < 1e5f)) return false;

    if (kind & 2) {
        // World to camera: the camera axes are the columns, and eye solves eye * R + t = 0.
        for (int c = 0; c < 3; ++c) p.ax[c] = {m[0][c], m[1][c], m[2][c]};
        p.eye = (p.ax[0] * t.x + p.ax[1] * t.y + p.ax[2] * t.z) * -1;
    } else {
        // Camera to world: rows are the axes, translation is the eye.
        for (int r = 0; r < 3; ++r) p.ax[r] = row[r];
        p.eye = t;
    }
    return true;
}

// Depth sign (+1 or -1) if this pose looks like an upright camera watching the player, else 0.
int watchesPlayer(const Pose& p, const Vec3& player, float maxAngle) {
    float dist = length(player - p.eye);
    if (!(dist >= 0.8f && dist <= 25)) return 0;
    if (!(p.ax[1].y > 0.3f) || !near0(p.ax[0].y, 0.1f)) return 0;  // no roll, not upside down
    Vec3 d = player + Vec3{0, 1.2f, 0} - p.eye;  // the camera aims around chest height
    float z = dot(d, p.ax[2]) / length(d);
    if (!(std::fabs(z) >= std::cos(maxAngle))) return 0;
    return z > 0 ? 1 : -1;
}

bool readProjection(const float* f, float& xs, float& ys) {
    if (f[15] != 0 || !(f[0] > 0.1f && f[0] < 10) || !(f[5] > 0.1f && f[5] < 10)) return false;
    for (int i : {1, 2, 3, 4, 6, 7, 8, 9, 12, 13})
        if (!near0(f[i], 1e-6f)) return false;
    // Depth goes to w through m[2][3] (or m[3][2] when stored transposed).
    if (!near0(std::fabs(f[11]) - 1, 1e-5f) && !near0(std::fabs(f[14]) - 1, 1e-5f)) return false;
    float aspect = f[5] / f[0], fov = 2 * std::atan(1 / f[5]);
    if (aspect < 1.0f || aspect > 3.6f || fov < 20 * kPi / 180 || fov > 100 * kPi / 180) return false;
    xs = f[0];
    ys = f[5];
    return true;
}

Camera toCamera(const Pose& p, int sign) {
    Camera c;
    c.eye = p.eye;
    c.right = p.ax[0];
    c.up = p.ax[1];
    c.fwd = p.ax[2] * static_cast<float>(sign);
    return c;
}
}  // namespace

bool cameraFromMatrix(const float* f, uint8_t kind, int sign, Camera& out) {
    Pose p;
    if (!decode(f, kind, p) || !(p.ax[1].y > 0)) return false;
    out = toCamera(p, sign);
    return true;
}

bool projectionScale(const float* f, float& xs, float& ys) { return readProjection(f, xs, ys); }

bool CameraLocator::scan(DWORD pid, const Vec3& player, const std::atomic<bool>& cancel,
                         std::vector<Candidate>& cams, std::vector<Projection>& projs,
                         size_t& bytesScanned) {
    // Own handle, so the main thread can detach from the game while this runs.
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    constexpr size_t kChunk = 16 << 20, kTail = 64;
    std::vector<uint8_t> buf(kChunk + kTail);
    bytesScanned = 0;

    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (!cancel && VirtualQueryEx(h, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi)) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        addr = end;
        // The camera is rewritten every frame, so only writable, committed, non-shared memory.
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        if (mbi.State != MEM_COMMIT || mbi.Type == MEM_MAPPED || !(mbi.Protect & writable) ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            continue;
        for (uintptr_t at = base; at < end && !cancel; at += kChunk) {
            SIZE_T want = std::min<uintptr_t>(kChunk + kTail, end - at), got = 0;
            ReadProcessMemory(h, reinterpret_cast<LPCVOID>(at), buf.data(), want, &got);
            if (got < 64) continue;
            bytesScanned += std::min<size_t>(got, kChunk);
            size_t last = std::min<size_t>(got - 64, kChunk - 16);
            for (size_t off = 0; off <= last; off += 16) {  // matrices are 16-byte aligned
                float f[16];
                std::memcpy(f, &buf[off], sizeof f);
                if (near0(f[15] - 1, 1e-4f)) {
                    for (uint8_t kind = 0; kind < 4; ++kind) {
                        Pose p;
                        if (!decode(f, kind, p)) continue;
                        int sign = watchesPlayer(p, player, kScanAngle);
                        if (sign && cams.size() < kMaxCandidates) cams.push_back({at + off, kind, sign});
                    }
                } else if (f[15] == 0 && f[0] > 0.1f) {
                    float xs, ys;
                    if (readProjection(f, xs, ys) && projs.size() < kMaxCandidates) {
                        Projection p{at + off, xs, ys};
                        std::memcpy(p.m, f, sizeof p.m);
                        projs.push_back(p);
                    }
                }
            }
        }
    }
    CloseHandle(h);
    return !cancel;
}

CameraLocator::~CameraLocator() { stopScan(); }

void CameraLocator::stopScan() {
    if (!job_) return;
    job_->cancel = true;
    if (job_->thread.joinable()) job_->thread.join();
    job_.reset();
}

void CameraLocator::reset() {
    stopScan();
    tracked_.clear();
    locked_ = 0;
    lastLocked_ = 0;
    nextScan_ = 0;
    scans_ = 0;
    proj_ = {};
    projValid_ = false;
    cam_.reset();
}

void CameraLocator::startScan(DWORD pid, const Vec3& player, uint64_t nowMs) {
    job_ = std::make_unique<ScanJob>();
    ScanJob* job = job_.get();
    scanStarted_ = nowMs;
    job->thread = std::thread([job, pid, player] {
        scan(pid, player, job->cancel, job->cams, job->projs, job->bytes);
        job->done = true;
    });
}

void CameraLocator::finishScan(float aspect, uint64_t nowMs) {
    job_->thread.join();
    ++scans_;
    lastScanBytes_ = job_->bytes;
    lastScanMs_ = nowMs - scanStarted_;

    // Keep the turn already watched for candidates that are still there.
    std::vector<Tracked> merged;
    for (const Candidate& c : job_->cams) {
        Tracked t{c};
        for (const Tracked& old : tracked_)
            if (old.c.addr == c.addr && old.c.kind == c.kind) t = old;
        merged.push_back(t);
    }
    tracked_ = std::move(merged);

    // The main camera's projection usually exists in several copies; take the most common one
    // shaped like the game's window. Shape first: shadow maps have projections of their own
    // (square, 90 degrees, one per cube face), and there are more of those than of the camera's.
    std::map<std::pair<int, int>, std::pair<int, Projection>> groups;
    for (const Projection& p : job_->projs) {
        auto& g = groups[{static_cast<int>(std::lround(p.ys * 1000)),
                          static_cast<int>(std::lround(p.ys / p.xs * 100))}];
        if (!g.first++) g.second = p;
    }
    // Exactly 90 degrees is what utility cameras use (shadows, reflections), not the player's.
    auto rank = [&](const Projection& p) {
        bool fits = std::fabs(p.ys / p.xs - aspect) < 0.03f * aspect;
        bool square90 = std::fabs(p.ys - 1) < 1e-3f;
        return (fits ? 2 : 0) + (square90 ? 0 : 1);
    };
    int best = 0, bestRank = -1;
    float bestGap = 0;
    candidates_.clear();
    for (auto& [key, g] : groups) {
        float gap = std::fabs(g.second.ys / g.second.xs - aspect);
        int r = rank(g.second);
        if (r >= 2) candidates_.push_back({2 * std::atan(1 / g.second.ys) * 180 / kPi, g.first});
        if (r > bestRank || (r == bestRank && (g.first > best || (g.first == best && gap < bestGap)))) {
            best = g.first;
            bestGap = gap;
            bestRank = r;
            proj_ = g.second;
        }
    }

    // The scan's own copy is valid: it stands in until a read of the slot is (the game reuses it).
    if (proj_.addr) {
        std::memcpy(projM_, proj_.m, sizeof projM_);
        projValid_ = true;
    }
    job_.reset();
    nextScan_ = nowMs + (tracked_.empty() ? kRetryMs : kRescanMs);
}

void CameraLocator::update(const Process& proc, const PlayerState& ps, float aspect,
                           float fallbackFov, bool forceFov, uint64_t nowMs) {
    if (job_ && job_->done) finishScan(aspect, nowMs);
    cam_.reset();
    if (!ps.valid) return;  // loading screen or menu: keep what we have, draw nothing

    float f[16];
    Pose p;
    if (locked_) {
        bool ok = proc.read(locked_, f, sizeof f) && decode(f, lockedKind_, p) &&
                  length(ps.pos - p.eye) < 40 && p.ax[1].y > 0;
        if (ok) {
            lastGood_ = nowMs;
            cam_ = toCamera(p, lockedSign_);
        } else if (nowMs - lastGood_ > 1500) {
            // Gone (area change, cutscene): scan again, and keep watching the same spot meanwhile.
            locked_ = 0;
            nextScan_ = nowMs + kRetryMs;
        }
    }

    if (!locked_) {
        auto lock = [&](const Candidate& c) {
            locked_ = lastLocked_ = c.addr;
            lockedKind_ = c.kind;
            lockedSign_ = c.sign;
            lastGood_ = nowMs;
            tracked_.clear();
            cam_ = toCamera(p, lockedSign_);
        };
        // The camera usually survives loading screens, so the last one is trusted straight away.
        if (lastLocked_ && proc.read(lastLocked_, f, sizeof f) && decode(f, lockedKind_, p) &&
            watchesPlayer(p, ps.pos, kScanAngle) == lockedSign_)
            lock({lastLocked_, lockedKind_, lockedSign_});
        for (size_t i = 0; !locked_ && i < tracked_.size();) {
            Tracked& t = tracked_[i];
            bool ok = proc.read(t.c.addr, f, sizeof f) && decode(f, t.c.kind, p) &&
                      watchesPlayer(p, ps.pos, kScanAngle) == t.c.sign;
            if (!ok) {
                tracked_.erase(tracked_.begin() + i);
                continue;
            }
            float yaw = std::atan2(p.ax[2].x, p.ax[2].z);
            if (!t.since) {
                t.since = nowMs;
                t.lastYaw = yaw;
            }
            t.turned += std::fabs(wrap(yaw - t.lastYaw));
            t.lastYaw = yaw;
            // Something that keeps the player in view while turning a full radian is the camera.
            if (t.turned >= kLockTurn && nowMs - t.since >= kLockTurnMs) lock(t.c);
            else ++i;
        }
        if (!locked_ && !job_ && nowMs >= nextScan_) startScan(proc.pid(), ps.pos, nowMs);
    }

    if (!cam_) return;
    float xs, ys;
    if (!forceFov && proj_.addr && proc.read(proj_.addr, f, sizeof f) && readProjection(f, xs, ys)) {
        proj_.xs = xs;
        proj_.ys = ys;
        std::memcpy(projM_, f, sizeof projM_);
        projValid_ = true;
    }
    if (!forceFov && proj_.addr) {
        cam_->xs = proj_.xs;
        cam_->ys = proj_.ys;
        cam_->fovFound = true;
    } else {
        cam_->ys = 1 / std::tan(fallbackFov / 2);
        cam_->xs = cam_->ys / aspect;
    }
}

std::wstring CameraLocator::status() const {
    wchar_t buf[160];
    if (locked_) {
        float fov = cam_ ? 2 * std::atan(1 / cam_->ys) * 180 / kPi : 0;
        swprintf(buf, 160, L"locked 0x%llX kind %u%ls, fov %.1f° %ls",
                 static_cast<unsigned long long>(locked_), lockedKind_, lockedSign_ < 0 ? L" (-z)" : L"",
                 fov, cam_ && cam_->fovFound ? L"(game)" : L"(config)");
        // The projections that fit the window, to tell which is the camera's: "43.0×3 90.0×12".
        std::wstring list = buf;
        list += L"\nfitting:";
        for (size_t i = 0; i < candidates_.size() && i < 6; ++i) {
            wchar_t c[32];
            swprintf(c, 32, L" %.1f×%d", candidates_[i].first, candidates_[i].second);
            list += c;
        }
        return list;
    } else if (job_) {
        swprintf(buf, 160, L"scanning memory (scan %d)", scans_ + 1);
    } else {
        swprintf(buf, 160, L"%zu candidates; scan %d took %llu ms over %zu MB", tracked_.size(),
                 scans_, static_cast<unsigned long long>(lastScanMs_), lastScanBytes_ >> 20);
    }
    return buf;
}

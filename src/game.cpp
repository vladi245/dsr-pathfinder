#include "game.h"

#include <cmath>
#include <map>

// Patterns and offsets come from DSR-Gadget (github.com/JKAnderson/DSR-Gadget, DSROffsets.cs).
namespace {
const wchar_t* kExe = L"DarkSoulsRemastered.exe";

const char* kWorldChrBaseAOB =
    "48 8B 05 ? ? ? ? 48 8B 48 68 48 85 C9 0F 84 ? ? ? ? 48 39 5E 10 0F 84 ? ? ? ? 48";
const char* kEventFlagsAOB = "48 8B 0D ? ? ? ? 99 33 C2 45 33 C0 2B C2 8D 50 F6";

constexpr ptrdiff_t kPlayerIns = 0x68;   // WorldChrBase -> player character
constexpr ptrdiff_t kChrMapData = 0x48;  // + chrDataBoost_ on 1.03 and later
constexpr ptrdiff_t kChrPosData = 0x28;
constexpr ptrdiff_t kPosAngle = 0x04;
constexpr ptrdiff_t kPosX = 0x10;

struct VersionInfo {
    const char* name;
    ptrdiff_t chrDataBoost;
};
const std::map<size_t, VersionInfo> kVersions = {
    {0x4869400, {"1.01", 0}},
    {0x496BE00, {"1.01.1", 0}},
    {0x37CB400, {"1.01.2", 0}},
    {0x3817800, {"1.03", 0x20}},
};
}  // namespace

DarkSouls::Status DarkSouls::connect() {
    if (ready()) return status_;
    if (!proc_.alive() && !proc_.attach(kExe)) {
        status_ = Status::NotRunning;
        return status_;
    }

    auto v = kVersions.find(proc_.moduleSize());
    chrDataBoost_ = v != kVersions.end() ? v->second.chrDataBoost : 0x20;  // newer = 1.03 layout

    // The exe is unpacked at runtime, so the patterns may not exist yet right after launch.
    uintptr_t wcb = proc_.findPattern(kWorldChrBaseAOB);
    uintptr_t ef = proc_.findPattern(kEventFlagsAOB);
    if (!wcb || !ef) {
        proc_.detach();  // drop the stale snapshot and rescan on the next attempt
        status_ = Status::Scanning;
        return status_;
    }
    worldChrBase_ = proc_.resolveRelative(wcb, 3, 7);
    eventFlags_ = proc_.resolveRelative(ef, 3, 7);
    status_ = Status::Ready;
    return status_;
}

void DarkSouls::disconnect() {
    proc_.detach();
    status_ = Status::NotRunning;
    worldChrBase_ = eventFlags_ = 0;
}

PlayerState DarkSouls::player() const {
    PlayerState s;
    if (!ready()) return s;
    uintptr_t pos =
        proc_.followChain(worldChrBase_, {0, kPlayerIns, kChrMapData + chrDataBoost_, kChrPosData});
    if (!pos) return s;  // main menu or loading screen

    float raw[7];  // angle at +0x4, xyz at +0x10..+0x18
    if (!proc_.read(pos, raw, sizeof raw)) return s;
    s.heading = raw[kPosAngle / 4];
    s.pos = {raw[kPosX / 4], raw[kPosX / 4 + 1], raw[kPosX / 4 + 2]};
    s.valid = std::isfinite(s.heading) && std::isfinite(s.pos.x) && std::isfinite(s.pos.y) &&
              std::isfinite(s.pos.z) && (s.pos.x != 0 || s.pos.y != 0 || s.pos.z != 0);
    return s;
}

std::optional<bool> DarkSouls::eventFlag(uint32_t id) const {
    static const std::map<uint32_t, uint32_t> groups = {
        {0, 0x00000}, {1, 0x00500}, {5, 0x05F00}, {6, 0x0B900}, {7, 0x11300}};
    static const std::map<uint32_t, uint32_t> areas = {
        {0, 0},    {100, 1},  {101, 2},  {102, 3},  {110, 4},  {120, 5},
        {121, 6},  {130, 7},  {131, 8},  {132, 9},  {140, 10}, {141, 11},
        {150, 12}, {151, 13}, {160, 14}, {170, 15}, {180, 16}, {181, 17}};

    if (!ready() || id > 99999999) return std::nullopt;
    // 8 digits: G AAA S NNN
    auto g = groups.find(id / 10000000);
    auto a = areas.find(id / 10000 % 1000);
    if (g == groups.end() || a == areas.end()) return std::nullopt;
    uint32_t section = id / 1000 % 10, number = id % 1000;
    uint32_t offset = g->second + a->second * 0x500 + section * 128 + (number - number % 32) / 8;

    uintptr_t flags = proc_.followChain(eventFlags_, {0, 0});
    if (!flags) return std::nullopt;
    auto word = proc_.read<uint32_t>(flags + offset);
    if (!word) return std::nullopt;
    return (*word & (0x80000000u >> (number % 32))) != 0;
}

HWND DarkSouls::window() const {
    struct Search {
        DWORD pid;
        HWND best;
        long bestArea;
    } search{proc_.pid(), nullptr, 0};
    if (!search.pid) return nullptr;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<Search*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != s->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
            RECT r;
            GetClientRect(h, &r);
            long area = (r.right - r.left) * (r.bottom - r.top);
            if (area > s->bestArea) {
                s->best = h;
                s->bestArea = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.best;
}

std::string DarkSouls::versionName() const {
    auto v = kVersions.find(proc_.moduleSize());
    if (v != kVersions.end()) return v->second.name;
    char buf[48];
    snprintf(buf, sizeof buf, "unknown (module size 0x%zX)", proc_.moduleSize());
    return buf;
}

#include "inworld.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>

#include "appdata.h"
#include "resource.h"

namespace fs = std::filesystem;

namespace {
bool sameName(const wchar_t* a, const std::wstring& b) {
    return _wcsicmp(a, b.c_str()) == 0;
}

// Which of our renderer and Direct3D the game has loaded.
void modulesOf(DWORD pid, const std::wstring& dllName, bool& hasD3D, bool& hasHook) {
    hasD3D = hasHook = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (sameName(me.szModule, L"d3d11.dll")) hasD3D = true;
        if (sameName(me.szModule, dllName)) hasHook = true;
    }
    CloseHandle(snap);
}

// The renderer's file name carries a fingerprint of its contents, so an updated app never
// collides with an older copy the game still has loaded.
std::wstring dllNameFor(std::string_view bytes) {
    uint32_t h = 2166136261u;
    for (unsigned char c : bytes) h = (h ^ c) * 16777619u;
    wchar_t buf[48];
    swprintf(buf, 48, L"DSRPathfinderHook-%08x.dll", h);
    return buf;
}

bool loadInto(DWORD pid, const fs::path& dll, std::wstring& error) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        error = L"no access to the game process";
        return false;
    }
    const std::wstring path = dll.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    if (remote && WriteProcessMemory(proc, remote, path.c_str(), bytes, nullptr)) {
        auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (HANDLE t = CreateRemoteThread(proc, nullptr, 0, load, remote, 0, nullptr)) {
            DWORD code = 0;
            ok = WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code != 0;
            CloseHandle(t);
            if (!ok) error = L"the game didn't load the renderer";
        } else {
            error = L"couldn't start the renderer in the game";
        }
    } else {
        error = L"couldn't pass the renderer to the game";
    }
    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}
}  // namespace

InWorld::~InWorld() {
    if (block_) UnmapViewOfFile(block_);
    if (mapping_) CloseHandle(mapping_);
}

bool InWorld::start() {
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(trailshare::Block),
                                  trailshare::kMappingName);
    if (!mapping_) return false;
    block_ = static_cast<trailshare::Block*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!block_) return false;
    block_->magic = trailshare::kMagic;
    block_->version = trailshare::kVersion;
    frame_ = std::make_unique<trailshare::Frame>();
    return true;
}

void InWorld::attach(DWORD pid, const fs::path& dir, uint64_t nowMs) {
    if (!block_ || pid == pid_ || nowMs < nextTry_) return;
    nextTry_ = nowMs + 3000;
    std::string_view bytes = appdata::embedded(IDR_HOOK);
    if (bytes.empty()) {
        error_ = L"the renderer isn't built in";
        return;
    }
    const std::wstring name = dllNameFor(bytes);
    bool hasD3D, hasHook;
    modulesOf(pid, name, hasD3D, hasHook);
    if (hasHook) {  // still there from an earlier run of the app
        pid_ = pid;
        return;
    }
    if (!hasD3D) return;  // the game hasn't started Direct3D yet

    // Put the DLL next to your settings, and clear out older versions of it.
    const fs::path dll = dir / name;
    std::error_code ec;
    if (!fs::exists(dll, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::wstring f = e.path().filename().wstring();
            if (f.rfind(L"DSRPathfinderHook-", 0) == 0) fs::remove(e.path(), ec);  // fails if in use: fine
        }
        FILE* f = _wfopen(dll.c_str(), L"wb");
        bool written = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        if (f) std::fclose(f);
        if (!written) {
            error_ = L"couldn't write " + name;
            return;
        }
    }
    if (tried_ == pid) return;  // one attempt per game run: a refusal won't change
    tried_ = pid;
    if (loadInto(pid, dll, error_)) {
        pid_ = pid;
        error_.clear();
        std::printf("In-world trail: renderer loaded into the game\n");
    } else {
        std::printf("In-world trail unavailable (%ls); the overlay draws it instead\n", error_.c_str());
    }
}

void InWorld::publish(const OverlayView& v, const CameraLocator::Source& cam, bool draw, bool occlusion,
                      uint64_t nowMs) {
    if (!block_) return;
    trailshare::Frame& f = *frame_;
    uint32_t n = static_cast<uint32_t>(std::min<size_t>(v.trail.size(), trailshare::kMaxPoints));
    f.draw = draw && v.camera && n >= 2 ? 1 : 0;
    f.style = static_cast<uint32_t>(v.moteStyle);
    f.occlusion = occlusion ? 1 : 0;
    f.camAddr = cam.camAddr;
    f.projAddr = cam.projAddr;
    f.camKind = cam.kind;
    f.camSign = cam.sign;
    f.xs = v.camera ? v.camera->xs : 1;
    f.ys = v.camera ? v.camera->ys : 1;
    f.endsAtGoal = v.trailEndsAtGoal ? 1 : 0;
    f.projValid = cam.proj ? 1 : 0;
    if (cam.proj) std::memcpy(f.proj, cam.proj, sizeof f.proj);
    f.trailBase = v.trailBase;
    f.strength = v.trailStrength;
    for (uint32_t i = 0; i < n; ++i) {
        f.points[i][0] = v.trail[i].x;
        f.points[i][1] = v.trail[i].y;
        f.points[i][2] = v.trail[i].z;
    }
    trailshare::write(*block_, f, n, nowMs);
}

bool InWorld::drawing(uint64_t nowMs) const {
    return block_ && pid_ && block_->hookVersion == trailshare::kVersion &&
           nowMs - block_->hookMs < trailshare::kHookTimeoutMs;
}

std::wstring InWorld::status(uint64_t nowMs) const {
    if (!block_) return L"off (no shared memory)";
    if (!pid_) return error_.empty() ? L"waiting for the game's Direct3D" : L"unavailable: " + error_;
    if (!drawing(nowMs)) return L"loaded, waiting for frames";
    wchar_t buf[sizeof block_->hookInfo + 1];
    swprintf(buf, sizeof block_->hookInfo + 1, L"%hs", block_->hookInfo);
    return buf;
}

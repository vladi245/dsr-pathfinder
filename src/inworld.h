#pragma once
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "camera.h"
#include "overlay.h"
#include "trailshare.h"

// The in-world trail: loads the renderer DLL (src/hook) into the game and feeds it the trail,
// so the motes are drawn into the game's own frames instead of the overlay window. When it
// can't (the DLL didn't load, or stopped answering), the overlay window draws them as before.
class InWorld {
public:
    ~InWorld();
    // Sets up the shared memory. False if that failed; the overlay window then does everything.
    bool start();
    // Call while attached to the game: loads the renderer into it once the game has created its
    // Direct3D device. `dir` is where the DLL file is put. Retries every few seconds.
    void attach(DWORD pid, const std::filesystem::path& dir, uint64_t nowMs);
    void detach() { pid_ = 0; }
    // Sends this tick's trail (or, with draw false, says to show nothing).
    void publish(const OverlayView& v, const CameraLocator::Source& cam, bool draw, bool occlusion,
                 uint64_t nowMs);
    // True while the renderer is drawing frames, so the overlay window leaves the motes to it.
    bool drawing(uint64_t nowMs) const;
    std::wstring status(uint64_t nowMs) const;

private:
    HANDLE mapping_ = nullptr;
    trailshare::Block* block_ = nullptr;
    std::unique_ptr<trailshare::Frame> frame_;
    DWORD pid_ = 0;       // the game process the renderer was loaded into
    DWORD tried_ = 0;     // ...or last tried
    uint64_t nextTry_ = 0;
    std::wstring error_;
};

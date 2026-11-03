#pragma once
#include "memory.h"

struct Vec3 {
    float x = 0, y = 0, z = 0;  // y is up
};

struct PlayerState {
    bool valid = false;
    Vec3 pos;
    float heading = 0;  // raw facing angle from the game, radians
};

// Dark Souls Remastered (Steam, x64). Reads player position and event flags; never writes.
class DarkSouls {
public:
    enum class Status { NotRunning, Scanning, Ready };

    // Call periodically while not Ready. Attaches and resolves pointers when the game is up.
    Status connect();
    bool ready() const { return status_ == Status::Ready && proc_.alive(); }
    void disconnect();

    PlayerState player() const;
    // Event flag state, or nullopt if the ID is malformed or memory isn't readable yet.
    std::optional<bool> eventFlag(uint32_t id) const;
    HWND window() const;
    std::string versionName() const;
    const Process& process() const { return proc_; }

private:
    Process proc_;
    Status status_ = Status::NotRunning;
    uintptr_t worldChrBase_ = 0;  // static slot holding the WorldChrBase pointer
    uintptr_t eventFlags_ = 0;    // static slot holding the event flag manager pointer
    ptrdiff_t chrDataBoost_ = 0x20;
};

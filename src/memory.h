#pragma once
#include <windows.h>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

// Read-only access to another process. Nothing in this project ever writes to the game.
class Process {
public:
    ~Process() { detach(); }

    bool attach(const wchar_t* exeName);
    void detach();
    bool alive() const;

    DWORD pid() const { return pid_; }
    uintptr_t moduleBase() const { return base_; }
    size_t moduleSize() const { return size_; }

    bool read(uintptr_t addr, void* out, size_t len) const;
    template <class T>
    std::optional<T> read(uintptr_t addr) const {
        T v{};
        if (!read(addr, &v, sizeof v)) return std::nullopt;
        return v;
    }

    // Scans the main module for a byte pattern like "48 8B 05 ? ? ? ?". Returns 0 if absent.
    uintptr_t findPattern(const std::string& pattern) const;
    // Target of a RIP-relative operand: instr + instrLen + disp32 stored at instr + dispOffset.
    uintptr_t resolveRelative(uintptr_t instr, int dispOffset, int instrLen) const;
    // p = [start + o0]; p = [p + o1]; ... Returns 0 if any link is null or unreadable.
    uintptr_t followChain(uintptr_t start, std::initializer_list<ptrdiff_t> offsets) const;

private:
    void loadImage() const;

    HANDLE h_ = nullptr;
    DWORD pid_ = 0;
    uintptr_t base_ = 0;
    size_t size_ = 0;
    mutable std::vector<uint8_t> image_;  // snapshot of the module used for pattern scans
};

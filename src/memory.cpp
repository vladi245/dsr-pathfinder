#include "memory.h"

#include <tlhelp32.h>

#include <algorithm>
#include <sstream>

bool Process::attach(const wchar_t* exeName) {
    detach();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exeName) == 0) {
            pid = pe.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    if (!pid) return false;

    // Fails briefly while the game is still starting up; the caller retries.
    HANDLE mods = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (mods == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    bool found = false;
    for (BOOL ok = Module32FirstW(mods, &me); ok; ok = Module32NextW(mods, &me)) {
        if (_wcsicmp(me.szModule, exeName) == 0) {
            found = true;
            break;
        }
    }
    CloseHandle(mods);
    if (!found) return false;

    h_ = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h_) return false;
    pid_ = pid;
    base_ = reinterpret_cast<uintptr_t>(me.modBaseAddr);
    size_ = me.modBaseSize;
    return true;
}

void Process::detach() {
    if (h_) CloseHandle(h_);
    h_ = nullptr;
    pid_ = 0;
    base_ = 0;
    size_ = 0;
    image_.clear();
    image_.shrink_to_fit();
}

bool Process::alive() const {
    DWORD code = 0;
    return h_ && GetExitCodeProcess(h_, &code) && code == STILL_ACTIVE;
}

bool Process::read(uintptr_t addr, void* out, size_t len) const {
    SIZE_T got = 0;
    return h_ && addr && ReadProcessMemory(h_, reinterpret_cast<LPCVOID>(addr), out, len, &got) &&
           got == len;
}

void Process::loadImage() const {
    image_.assign(size_, 0);
    uintptr_t addr = base_, end = base_ + size_;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(h_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi)) break;
        uintptr_t regionEnd =
            std::min(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, end);
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            ReadProcessMemory(h_, reinterpret_cast<LPCVOID>(addr), &image_[addr - base_],
                              regionEnd - addr, nullptr);
        }
        addr = regionEnd;
    }
}

uintptr_t Process::findPattern(const std::string& pattern) const {
    std::vector<int> bytes;  // -1 = wildcard
    std::istringstream in(pattern);
    std::string tok;
    while (in >> tok) bytes.push_back(tok[0] == '?' ? -1 : std::stoi(tok, nullptr, 16));

    if (image_.empty()) loadImage();
    if (bytes.empty() || image_.size() < bytes.size()) return 0;

    const size_t last = image_.size() - bytes.size();
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        while (j < bytes.size() && (bytes[j] < 0 || image_[i + j] == bytes[j])) ++j;
        if (j == bytes.size()) return base_ + i;
    }
    return 0;
}

uintptr_t Process::resolveRelative(uintptr_t instr, int dispOffset, int instrLen) const {
    auto disp = read<int32_t>(instr + dispOffset);
    if (!disp) return 0;
    return instr + instrLen + *disp;
}

uintptr_t Process::followChain(uintptr_t start, std::initializer_list<ptrdiff_t> offsets) const {
    uintptr_t p = start;
    for (ptrdiff_t off : offsets) {
        auto next = read<uintptr_t>(p + off);
        if (!next || !*next) return 0;
        p = *next;
    }
    return p;
}

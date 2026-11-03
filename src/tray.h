#pragma once
#include <windows.h>

#include <string>

// The app's icon in the notification area: its tooltip shows the status, right-click (or
// left-click) opens the menu, and notifications replace what used to go to a console.
class Tray {
public:
    enum Command { kNone, kToggleOverlay, kOpenSettings, kOpenFolder, kLaunchGame, kQuit };

    bool create(HINSTANCE inst, HICON icon);
    ~Tray();
    void setStatus(const std::wstring& text);  // tooltip and the menu's first line
    void notify(const std::wstring& title, const std::wstring& text);
    void setState(bool overlayShown, bool gameRunning);  // for the menu's ticks and greying
    Command takeCommand();  // the last menu choice, once

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void showMenu();

    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_{};
    std::wstring status_ = L"Starting...";
    bool overlayShown_ = true, gameRunning_ = false;
    Command pending_ = kNone;
};

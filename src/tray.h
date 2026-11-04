#pragma once
#include <windows.h>

#include <string>

// The app's icon in the notification area: its tooltip shows the status, right-click (or
// left-click) opens the menu, and notifications replace what used to go to a console. The
// menu holds the settings too, so nobody has to edit config.ini.
class Tray {
public:
    enum Command { kNone, kToggleOverlay, kOpenSettings, kOpenFolder, kLaunchGame, kQuit, kSetting, kRecalibrate, kReload };

    // What the settings submenus show ticked (the values as config.ini spells them).
    struct Settings {
        bool trail = true, inWorld = true, occlusion = true, calm = true, launchGame = true;
        std::string style = "stream";   // trail_style
        std::string compass = "auto";   // auto, 1, 0
    };

    bool create(HINSTANCE inst, HICON icon);
    ~Tray();
    void setStatus(const std::wstring& text);  // tooltip and the menu's first line
    void notify(const std::wstring& title, const std::wstring& text);
    void setState(bool overlayShown, bool gameRunning);  // for the menu's ticks and greying
    void setSettings(const Settings& s) { settings_ = s; }
    Command takeCommand();  // the last menu choice, once
    // After kSetting: the config.ini key and value that were picked.
    const std::string& settingKey() const { return key_; }
    const std::string& settingValue() const { return value_; }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void showMenu();

    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_{};
    std::wstring status_ = L"Starting...";
    bool overlayShown_ = true, gameRunning_ = false;
    Settings settings_;
    Command pending_ = kNone;
    std::string key_, value_;
};

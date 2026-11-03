#include "tray.h"

#include <shellapi.h>

namespace {
const wchar_t* kClass = L"DSRPathfinderTray";
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kId = 1;
enum MenuId : UINT { kMenuStatus = 100, kMenuToggle, kMenuSettings, kMenuFolder, kMenuLaunch, kMenuQuit };

void copyTo(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy_s(dst, cap, s.c_str(), _TRUNCATE);
}
}  // namespace

bool Tray::create(HINSTANCE inst, HICON icon) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    // A hidden window: it receives the icon's clicks and is never shown. (Not message-only:
    // the menu needs a window that can take the foreground to close when you click away.)
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"DSR Pathfinder", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            inst, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    nid_.cbSize = sizeof nid_;
    nid_.hWnd = hwnd_;
    nid_.uID = kId;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = kTrayMsg;
    nid_.hIcon = icon;
    copyTo(nid_.szTip, ARRAYSIZE(nid_.szTip), L"DSR Pathfinder");
    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) return false;
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    return true;
}

Tray::~Tray() {
    if (hwnd_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        DestroyWindow(hwnd_);
    }
}

void Tray::setStatus(const std::wstring& text) {
    if (text == status_ || !hwnd_) return;
    status_ = text;
    nid_.uFlags = NIF_TIP | NIF_SHOWTIP;
    copyTo(nid_.szTip, ARRAYSIZE(nid_.szTip), L"DSR Pathfinder: " + text);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::notify(const std::wstring& title, const std::wstring& text) {
    if (!hwnd_) return;
    nid_.uFlags = NIF_INFO;
    nid_.dwInfoFlags = NIIF_NONE | NIIF_RESPECT_QUIET_TIME;
    copyTo(nid_.szInfoTitle, ARRAYSIZE(nid_.szInfoTitle), title);
    copyTo(nid_.szInfo, ARRAYSIZE(nid_.szInfo), text);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::setState(bool overlayShown, bool gameRunning) {
    overlayShown_ = overlayShown;
    gameRunning_ = gameRunning;
}

Tray::Command Tray::takeCommand() {
    Command c = pending_;
    pending_ = kNone;
    return c;
}

void Tray::showMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuStatus, status_.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (overlayShown_ ? MF_CHECKED : 0), kMenuToggle, L"Show the overlay\tF9");
    AppendMenuW(menu, MF_STRING | (gameRunning_ ? MF_GRAYED : 0), kMenuLaunch, L"Launch Dark Souls Remastered");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...");
    AppendMenuW(menu, MF_STRING, kMenuFolder, L"Open the app's folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);  // so the menu closes when you click elsewhere
    UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (choice) {
        case kMenuToggle: pending_ = kToggleOverlay; break;
        case kMenuSettings: pending_ = kOpenSettings; break;
        case kMenuFolder: pending_ = kOpenFolder; break;
        case kMenuLaunch: pending_ = kLaunchGame; break;
        case kMenuQuit: pending_ = kQuit; break;
        default: break;
    }
}

LRESULT CALLBACK Tray::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg == kTrayMsg) {
        UINT event = LOWORD(lp);
        if (event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT) self->showMenu();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

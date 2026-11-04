#pragma once
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "camera.h"
#include "motes.h"

namespace Gdiplus {
class Graphics;
}


// Everything the overlay draws in one frame. Built by main, drawn by Overlay.
struct OverlayView {
    // Compass
    bool showCompass = false;
    bool arrived = false;
    float relAngle = 0;     // radians, 0 = straight ahead, positive = to the right
    float distance = 0;     // to the next waypoint
    float routeLeft = 0;    // along the remaining waypoints
    float heightDiff = 0;   // target.y - player.y

    // Route trail drawn into the world, when the camera is known
    std::optional<Camera> camera;
    std::vector<Vec3> trail;    // from the player along the route to the next destination
    bool trailEndsAtGoal = false;
    float goalHeight = 0;       // destination.y - player.y
    float goalLeft = 0;         // along the trail
    bool particles = true;      // motes; false = a solid line
    int moteStyle = 0;          // MoteStyle
    bool motesInGame = false;   // the in-game renderer draws the motes (see inworld.h)
    float trailBase = 0;        // distance along the step's route of trail[1], to pin motes in place
    float timeSec = 0;          // drives the motes' drift
    // 1 = fully shown; lower while you're calmly following the route (see main.cpp).
    float trailStrength = 1;
    float panelStrength = 1;    // 0 = only the dimmed area and instruction

    // Step panel
    std::wstring header;    // "Step 3 / 40"
    std::wstring title;
    std::wstring body;
    std::wstring warning;   // missable / point-of-no-return text
    std::wstring progress;  // "Waypoint 2 / 7"
    std::wstring hint;      // muted line under the panel
    std::wstring notice;    // short-lived banner, e.g. "Waypoint recorded"
    std::vector<std::wstring> debug;
};

// Draws a frame of the overlay; used by Overlay::render and the offline tests.
void paintOverlay(Gdiplus::Graphics& g, int w, int h, const OverlayView& view);

// A click-through, always-on-top, per-pixel-alpha window laid over the game's client area.
class Overlay {
public:
    ~Overlay();
    bool create(HINSTANCE inst);
    void hide();
    // clientRect is in screen coordinates.
    void render(const RECT& clientRect, const OverlayView& view);

private:
    void ensureSurface(int w, int h);

    HWND hwnd_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    ULONG_PTR gdiplusToken_ = 0;
    bool visible_ = false;
};

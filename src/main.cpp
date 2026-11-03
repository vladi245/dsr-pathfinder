// DSR Pathfinder: an achievement-guide overlay for Dark Souls Remastered.
// Reads the player's position from the game (read-only) and points at the next guide waypoint.

#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>

#include "appdata.h"
#include "camera.h"
#include "game.h"
#include "guide.h"
#include "nav.h"
#include "overlay.h"
#include "resource.h"
#include "route.h"
#include "textfile.h"
#include "tray.h"

namespace fs = std::filesystem;
using textfile::widen;

namespace {

// Further than this from the guide's route (metres, height counting triple), the trail routes
// from where you are instead.
constexpr float kOffRoute = 25;

struct Key {
    int vk;
    bool down = false;
    bool pressed() {
        bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool edge = now && !down;
        down = now;
        return edge;
    }
};

bool shiftHeld() { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, f);
    vswprintf(buf, 512, f, args);
    va_end(args);
    return buf;
}

class App {
public:
    bool init();
    bool tick();  // true while the trail is on screen and needs a high frame rate
    bool quitting() const { return quit_; }

private:
    void loadGuide();
    void handleTray();
    void launchGame();
    void goToStep(size_t s, bool forward);
    void notify(const std::wstring& text, uint64_t ms = 2500);
    void handleKeys(const PlayerState& ps);
    OverlayView buildView(const PlayerState& ps, const std::optional<Camera>& cam);
    const Step* current() const {
        return step_ < guide_.steps().size() ? &guide_.steps()[step_] : nullptr;
    }
    // True once every flag of the step is on; nullopt if it has none or memory isn't readable.
    std::optional<bool> flagOf(size_t s) const {
        const auto& steps = guide_.steps();
        if (s >= steps.size() || steps[s].flags.empty()) return std::nullopt;
        bool all = true;
        for (uint32_t id : steps[s].flags) {
            auto f = ds_.eventFlag(id);
            if (!f) return std::nullopt;
            all = all && *f;
        }
        return all;
    }

    fs::path dir_;
    textfile::KeyValueFile config_, progress_;
    Guide guide_;
    DarkSouls ds_;
    Overlay overlay_;
    HeadingCalibrator calib_;
    RouteFollower route_;
    CameraLocator camera_;
    NavMesh nav_;       // the game's navigation meshes, for routing from where you are
    LiveRoute live_;
    Tray tray_;
    bool quit_ = false, launchOnStart_ = true, launched_ = false;

    size_t step_ = 0;
    std::optional<bool> stepFlag_;  // last seen state of the current step's flag
    bool visible_ = true, debug_ = false, mirror_ = false;
    bool trailOn_ = true, forceFov_ = false;
    int compassMode_ = -1;  // -1 auto (only without the trail), 0 never, 1 always
    float radius_ = 4, fov_ = 0.785f;
    std::wstring notice_;
    uint64_t noticeUntil_ = 0, nextConnect_ = 0;
    DarkSouls::Status lastStatus_ = DarkSouls::Status::Ready;  // forces the first status print
    std::wstring lastFrameKey_;
    RECT lastRect_{};

    // Calm mode: the overlay steps back while you're walking the route and comes forward when
    // you stop, arrive, change step or leave the route.
    bool calmOn_ = true, offRoute_ = false, particles_ = true;
    int moteStyle_ = kMotePulse;
    uint64_t stepShownMs_ = 0, stillSinceMs_ = 0, lastCalmMs_ = 0;
    Vec3 anchor_{};  // where you were when you last counted as moving
    float trailStrength_ = 1, panelStrength_ = 1;

    Key kPrev{VK_F5}, kNext{VK_F6}, kRecord{VK_F7}, kUndo{VK_F8}, kToggle{VK_F9},
        kReload{VK_F10}, kDebug{VK_F11};
};

bool App::init() {
    // Settings, progress and your copy of the guide live in %APPDATA%\DSR Pathfinder (or next to
    // the exe for a portable copy); the built-in defaults are put there on first run.
    dir_ = appdata::folder();
    appdata::prepare(dir_);
    if (!config_.load(dir_ / "config.ini")) std::printf("config.ini missing, using defaults\n");
    progress_.load(dir_ / "progress.txt");  // created on first save
    launchOnStart_ = config_.getInt("launch_game", 1) != 0;

    radius_ = config_.getFloat("waypoint_radius", 4.0f);
    mirror_ = config_.getInt("arrow_mirror", 0) != 0;
    trailOn_ = config_.getInt("trail", 1) != 0;
    calmOn_ = config_.getInt("calm", 1) != 0;
    std::string style = config_.get("trail_style", "pulse");
    particles_ = style != "line";
    // pulse unless another style is named (older settings say "particles").
    moteStyle_ = style == "embers" ? kMoteEmbers : style == "wisps" ? kMoteWisps
               : style == "fireflies" ? kMoteFireflies : kMotePulse;
    std::string compass = config_.get("compass", "auto");
    compassMode_ = compass == "1" ? 1 : compass == "0" ? 0 : -1;
    float fovDeg = config_.getFloat("camera_fov", 0);
    forceFov_ = fovDeg > 0;
    fov_ = (forceFov_ ? fovDeg : 45.0f) * 3.14159265f / 180;
    int sign = config_.getInt("heading_sign", 0);
    if (sign == 1 || sign == -1) calib_.set(sign, config_.getFloat("heading_offset", 0));

    loadGuide();
    // A navmesh.bin in the folder (one you regenerated) wins over the one built in.
    std::error_code ec;
    bool nav = fs::exists(dir_ / "navmesh.bin", ec) ? nav_.load(dir_ / "navmesh.bin")
                                                    : nav_.load(appdata::embedded(IDR_NAVMESH));
    std::printf(nav ? "Navmesh loaded: the trail routes from wherever you are\n"
                    : "No navmesh: off the guide's route the trail is a straight line\n");
    step_ = std::min<size_t>(std::max(progress_.getInt("step", 1), 1) - 1,
                             guide_.steps().empty() ? 0 : guide_.steps().size() - 1);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!overlay_.create(inst)) {
        std::printf("Couldn't create the overlay window\n");
        return false;
    }
    tray_.create(inst, LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP)));
    tray_.notify(L"DSR Pathfinder is running",
                 L"The guide appears over Dark Souls Remastered. F5/F6 change step, F9 hides it. "
                 L"Right-click this icon for settings or to quit.");
    return true;
}

void App::launchGame() {
    // Through Steam, which starts it the way the library's Play button does.
    ShellExecuteW(nullptr, L"open", L"steam://rungameid/570940", nullptr, nullptr, SW_SHOWNORMAL);
    tray_.notify(L"Starting Dark Souls Remastered", L"Through Steam. The guide appears once you're in game.");
}

void App::handleTray() {
    tray_.setState(visible_, ds_.ready());
    switch (tray_.takeCommand()) {
        case Tray::kToggleOverlay: visible_ = !visible_; break;
        case Tray::kOpenSettings: {
            std::wstring file = L"\"" + (dir_ / "config.ini").wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"notepad.exe", file.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case Tray::kOpenFolder: ShellExecuteW(nullptr, L"open", dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Tray::kLaunchGame: launchGame(); break;
        case Tray::kQuit: quit_ = true; break;
        default: break;
    }
}

void App::loadGuide() {
    std::string file = config_.get("guide", "guide.txt");
    std::string err;
    if (!guide_.load(dir_ / fs::u8path(file), err)) std::printf("Guide problem: %s\n", err.c_str());
    std::printf("Loaded %zu steps from %s\n", guide_.steps().size(), file.c_str());
}

void App::goToStep(size_t s, bool forward) {
    const size_t n = guide_.steps().size();
    if (!n) return;
    s = std::min(s, n - 1);
    // Moving forward skips steps the game already reports as done.
    while (forward && s + 1 < n && flagOf(s) == true) ++s;
    step_ = s;
    route_.reset();
    stepShownMs_ = GetTickCount64();
    stepFlag_ = flagOf(step_);
    progress_.set("step", std::to_string(step_ + 1));
    progress_.save();
}

void App::notify(const std::wstring& text, uint64_t ms) {
    notice_ = text;
    noticeUntil_ = GetTickCount64() + ms;
}

void App::handleKeys(const PlayerState& ps) {
    const size_t n = guide_.steps().size();
    if (kPrev.pressed() && step_ > 0) goToStep(step_ - 1, false);
    if (kNext.pressed() && step_ + 1 < n) goToStep(step_ + 1, true);
    if (kToggle.pressed()) visible_ = !visible_;
    if (kDebug.pressed()) {
        if (shiftHeld()) {
            calib_.reset();
            config_.set("heading_sign", "0");
            config_.save();
            notify(L"Recalibrating compass: run in a straight line");
        } else {
            debug_ = !debug_;
        }
    }
    if (kReload.pressed()) {
        loadGuide();
        goToStep(step_, false);
        notify(L"Guide reloaded");
    }
    if (kRecord.pressed()) {
        if (!ps.valid || !current()) {
            notify(L"Can't record: no character position");
        } else if (guide_.addWaypoint(step_, ps.pos)) {
            notify(fmt(L"Waypoint %zu recorded", current()->waypoints.size()));
        } else {
            notify(L"Couldn't write the guide file");
        }
    }
    if (kUndo.pressed()) {
        if (guide_.removeLastWaypoint(step_)) {
            route_.reset();
            notify(L"Last waypoint removed");
        }
    }
}

OverlayView App::buildView(const PlayerState& ps, const std::optional<Camera>& cam) {
    OverlayView v;
    offRoute_ = false;
    const Step* st = current();
    const size_t n = guide_.steps().size();
    if (!st) {
        v.title = L"No guide loaded";
        v.body = L"Put guide.txt next to the exe (or set guide = in config.ini), then press F10.";
        return v;
    }

    v.header = fmt(L"Step %zu / %zu", step_ + 1, n);
    v.title = widen(st->title);
    v.body = widen(st->body);
    v.warning = widen(st->warning);

    // A step without a spot of its own ("dodge the barrel trap") leads on to the next one that has.
    const Step* rs = st;
    size_t ahead = step_;
    while (rs->waypoints.empty() && ahead + 1 < n) rs = &guide_.steps()[++ahead];

    std::wstring status;
    const auto& wps = rs->waypoints;
    if (!ps.valid) {
        status = L"Waiting for your character to load…";
    } else if (wps.empty()) {
        status = L"No location for this step: press F7 where it happens to mark it.";
    } else {
        route_.update(*rs, ps.pos, rs->radius > 0 ? rs->radius : radius_);
        size_t idx = route_.index();
        bool arrived = route_.arrived(*rs);
        const Vec3& target = wps[std::min(idx, wps.size() - 1)];

        const bool trail = trailOn_ && cam && !arrived;
        v.showCompass = compassMode_ == 1 || (compassMode_ == -1 && !trail);
        v.showCompass = v.showCompass && (cam || calib_.done());
        v.arrived = arrived;
        v.distance = horizontalDist(ps.pos, target);
        v.heightDiff = target.y - ps.pos.y;
        v.routeLeft = v.distance;
        for (size_t i = idx; i + 1 < wps.size(); ++i) v.routeLeft += horizontalDist(wps[i], wps[i + 1]);
        if (cam) {
            // Relative to where the camera looks, which is what "ahead" means on screen.
            Vec3 d = target - ps.pos;
            float x = d.x * cam->right.x + d.z * cam->right.z, z = d.x * cam->fwd.x + d.z * cam->fwd.z;
            v.relAngle = std::atan2(x, z);
        } else {
            float facing = calib_.sign() * ps.heading + calib_.offset();
            v.relAngle = wrapAngle(bearing(ps.pos, target) - facing) * (mirror_ ? -1 : 1);
        }
        if (trail) {
            // From your feet along the route to the next destination.
            v.camera = cam;
            size_t g = idx;
            while (g + 1 < wps.size() && !rs->isGoal[g]) ++g;
            // Off the guide's route (back at a bonfire after dying, say): route from where you
            // are with the game's own navigation meshes instead of a straight line to the route.
            float offBy = std::numeric_limits<float>::infinity();
            for (size_t i = idx; i <= g; ++i) {
                float dy = (ps.pos.y - wps[i].y) * 3;
                offBy = std::min(offBy, std::hypot(horizontalDist(ps.pos, wps[i]), dy));
            }
            const std::vector<Vec3>* live = nullptr;
            offRoute_ = offBy > kOffRoute;
            if (nav_.loaded() && offRoute_) {
                live_.request(nav_, ps.pos, wps[g], GetTickCount64());
                if (live_.points().size() >= 2 && length(live_.target() - wps[g]) < 1) live = &live_.points();
            } else {
                live_.clear();
            }
            v.trail.push_back(ps.pos);
            if (live) {
                // The route began where you were when it was worked out: join it where you are now.
                size_t from = 0;
                float best = std::numeric_limits<float>::infinity();
                for (size_t i = 0; i < live->size(); ++i) {
                    float d = length((*live)[i] - ps.pos);
                    if (d < best) {
                        best = d;
                        from = i;
                    }
                }
                v.trail.insert(v.trail.end(), live->begin() + from + 1, live->end());
                if (v.trail.size() < 2) v.trail.push_back(wps[g]);
            } else {
                for (size_t i = idx; i <= g; ++i) v.trail.push_back(wps[i]);
                // How far along the step's route the trail picks up, so the motes stay put.
                for (size_t i = 0; i < idx && i + 1 < wps.size(); ++i) v.trailBase += length(wps[i + 1] - wps[i]);
            }
            v.trailEndsAtGoal = rs->isGoal[g];
            v.goalHeight = wps[g].y - ps.pos.y;
            for (size_t i = 0; i + 1 < v.trail.size(); ++i) v.goalLeft += length(v.trail[i + 1] - v.trail[i]);
        }
        size_t goals = 0, goalsPassed = 0;
        for (size_t i = 0; i < rs->isGoal.size(); ++i) {
            if (!rs->isGoal[i]) continue;
            ++goals;
            if (i < idx) ++goalsPassed;
        }
        v.progress = arrived     ? L"arrived"
                     : goals > 1 ? fmt(L"destination %zu / %zu", goalsPassed + 1, goals)
                                 : L"";

        if (trailOn_ && !cam && !camera_.locked())
            status = L"Finding the camera: turn it around (mouse or right stick) for a moment";
        else if (!calib_.done() && !cam)
            status = fmt(L"Calibrating compass: run in a straight line for a few seconds (%d%%)",
                         calib_.percent());
        else if (ahead != step_)
            status = arrived ? fmt(L"At step %zu's spot. Press F6 once this step is done.", ahead + 1)
                             : fmt(L"No exact spot for this step: the trail leads on to step %zu.", ahead + 1);
        else if (arrived && !st->flags.empty())
            status = L"Do the thing here; the step ticks off by itself.";
        else if (arrived)
            status = L"Done here? Press F6 for the next step.";
    }
    v.hint = status.empty() ? L"" : status + L"\n";
    v.hint += L"F5 back · F6 next · F7 add waypoint · F8 undo · F9 hide";

    if (GetTickCount64() < noticeUntil_) v.notice = notice_;

    if (debug_) {
        v.debug.push_back(fmt(L"game      %hs", ds_.versionName().c_str()));
        if (ps.valid) {
            v.debug.push_back(fmt(L"pos       %.2f, %.2f, %.2f", ps.pos.x, ps.pos.y, ps.pos.z));
            v.debug.push_back(fmt(L"heading   %.3f rad (raw)", ps.heading));
        }
        v.debug.push_back(fmt(L"calib     %ls sign %+d offset %.3f", calib_.done() ? L"done" : L"running",
                              calib_.sign(), calib_.offset()));
        for (uint32_t id : st->flags) {
            auto f = ds_.eventFlag(id);
            v.debug.push_back(fmt(L"flag      %u = %ls", id, !f ? L"unreadable" : *f ? L"ON" : L"off"));
        }
        if (v.showCompass) v.debug.push_back(fmt(L"rel       %.1f deg", v.relAngle * 57.2958f));
        if (trailOn_) v.debug.push_back(L"camera    " + camera_.status());
        if (cam)
            v.debug.push_back(fmt(L"eye       %.2f, %.2f, %.2f  fwd %.2f, %.2f, %.2f", cam->eye.x,
                                  cam->eye.y, cam->eye.z, cam->fwd.x, cam->fwd.y, cam->fwd.z));
    }
    return v;
}

bool App::tick() {
    uint64_t now = GetTickCount64();
    handleTray();

    if (!ds_.ready()) {
        overlay_.hide();
        camera_.reset();
        if (now < nextConnect_) return false;
        nextConnect_ = now + 2000;
        auto status = ds_.connect();
        if (status != lastStatus_) {
            lastStatus_ = status;
            if (status == DarkSouls::Status::NotRunning) {
                std::printf("Waiting for DarkSoulsRemastered.exe...\n");
                tray_.setStatus(L"Waiting for Dark Souls Remastered");
            } else if (status == DarkSouls::Status::Scanning) {
                std::printf("Game found, waiting for it to finish starting...\n");
                tray_.setStatus(L"Dark Souls Remastered is starting");
            } else {
                std::printf("Attached to Dark Souls Remastered %s\n", ds_.versionName().c_str());
                tray_.setStatus(L"Guiding you in Dark Souls Remastered");
            }
        }
        // Run it and it works: start the game if it isn't running yet (once; launch_game = 0 stops it).
        if (status == DarkSouls::Status::NotRunning && launchOnStart_ && !launched_) {
            launched_ = true;
            launchGame();
        }
        if (status == DarkSouls::Status::Ready) goToStep(step_, false);
        return false;
    }

    HWND game = ds_.window();
    PlayerState ps = ds_.player();
    calib_.feed(ps, now);
    if (calib_.done() && config_.getInt("heading_sign", 0) == 0) {
        config_.set("heading_sign", std::to_string(calib_.sign()));
        config_.set("heading_offset", std::to_string(calib_.offset()));
        config_.save();
        notify(L"Compass calibrated");
    }

    bool focused = game && GetForegroundWindow() == game && !IsIconic(game);
    if (focused) handleKeys(ps);

    RECT rc{};
    POINT tl{0, 0};
    if (game) {
        GetClientRect(game, &rc);
        ClientToScreen(game, &tl);
    }
    OffsetRect(&rc, tl.x, tl.y);
    if (trailOn_ && focused && rc.bottom > rc.top) {
        float aspect = static_cast<float>(rc.right - rc.left) / (rc.bottom - rc.top);
        camera_.update(ds_.process(), ps, aspect, fov_, forceFov_, now);
    }
    const std::optional<Camera> noCamera;
    const std::optional<Camera>& cam = trailOn_ && focused ? camera_.camera() : noCamera;

    // Auto-advance when the current step's event flag turns on while you're on it.
    auto flag = flagOf(step_);
    if (flag) {
        if (stepFlag_ == false && *flag) {
            if (step_ + 1 < guide_.steps().size()) {
                notify(L"Step complete");
                goToStep(step_ + 1, true);
            } else {
                notify(L"Guide complete. Praise the sun!", 6000);
            }
        }
        stepFlag_ = flagOf(step_);
    }

    if (!focused || !visible_) {
        overlay_.hide();
        lastFrameKey_.clear();
        return false;
    }

    OverlayView v = buildView(ps, cam);

    // Calm: walking along the route, the trail softens and the panel dims to its instruction.
    // Standing still for a moment, arriving, a new step or being off the route bring them back.
    if (ps.valid && length(ps.pos - anchor_) > 0.6f) {
        anchor_ = ps.pos;
        stillSinceMs_ = now;
    }
    const uint64_t still = now - stillSinceMs_;
    const bool alert = !calmOn_ || v.arrived || debug_ || offRoute_;
    float trailTarget = alert || still > 1500 ? 1.0f : 0.45f;
    float panelTarget = alert || still > 2500 || now - stepShownMs_ < 10000 ? 1.0f : 0.0f;
    float ease = std::min(1.0f, (now - lastCalmMs_) / 450.0f);  // about half a second to settle
    lastCalmMs_ = now;
    trailStrength_ += (trailTarget - trailStrength_) * ease;
    panelStrength_ += (panelTarget - panelStrength_) * ease;
    v.trailStrength = trailStrength_;
    v.panelStrength = panelStrength_;
    v.particles = particles_;
    v.moteStyle = moteStyle_;
    v.timeSec = static_cast<float>(now % 3600000) / 1000.0f;  // wraps hourly, keeping float precision

    // Skip redrawing identical frames; the arrow is quantised to half a degree.
    std::wstring key = fmt(L"%d|%d|%.0f|%.1f|%.0f|%.0f|%.2f|%.2f|", v.showCompass, v.arrived,
                           v.relAngle * 360, v.distance, v.heightDiff, v.routeLeft, v.trailStrength,
                           v.panelStrength) +
                       v.header + v.title + v.body + v.warning + v.progress + v.hint + v.notice;
    for (auto& d : v.debug) key += d;
    if (v.camera) {
        const Camera& c = *v.camera;
        key += fmt(L"|%.3f %.3f %.3f %.4f %.4f %.4f %.4f %.2f %.2f %.2f", c.eye.x, c.eye.y, c.eye.z,
                   c.fwd.x, c.fwd.y, c.fwd.z, c.ys, ps.pos.x, ps.pos.y, ps.pos.z);
    }
    bool animating = v.camera.has_value();
    if (animating && v.particles && v.trail.size() >= 2) key += fmt(L"|%.3f", v.timeSec);  // the motes drift
    if (key == lastFrameKey_ && EqualRect(&rc, &lastRect_)) return animating;
    lastFrameKey_ = key;
    lastRect_ = rc;
    overlay_.render(rc, v);
    return animating;
}

}  // namespace

int main() {
    // One copy at a time: a second one would draw a second overlay.
    CreateMutexW(nullptr, TRUE, L"DSRPathfinder.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"DSR Pathfinder is already running: its icon is by the clock.", L"DSR Pathfinder",
                    MB_ICONINFORMATION);
        return 0;
    }
    // No console window: status lines go to log.txt in the app's folder.
    _wfreopen((appdata::folder() / "log.txt").c_str(), L"w", stdout);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    App app;
    if (!app.init()) {
        MessageBoxW(nullptr, L"DSR Pathfinder couldn't start. log.txt in its folder says why.", L"DSR Pathfinder",
                    MB_ICONERROR);
        return 1;
    }
    timeBeginPeriod(1);  // accurate short sleeps while the trail follows the camera
    while (!app.quitting()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(app.tick() ? 8 : 33);
    }
    timeEndPeriod(1);
    return 0;
}

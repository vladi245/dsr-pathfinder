#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
using std::max;
using std::min;
#include <gdiplus.h>

namespace gp = Gdiplus;

namespace {
const wchar_t* kClass = L"DSRPathfinderOverlay";
const float kPi = 3.14159265f;

// The look: no boxes. Old-book serif type on shadow that fades into the scene, hairline rules
// with a diamond, in the bone and ember colours of a bonfire-lit menu.
const gp::Color kBone(255, 226, 218, 198);     // body text
const gp::Color kAsh(255, 156, 146, 130);      // labels and hints
const gp::Color kEmber(255, 230, 176, 98);     // headings, trail, markers
const gp::Color kBlood(255, 222, 104, 82);     // warnings
const gp::Color kVerdant(255, 176, 208, 146);  // arrived
const gp::Color kSoot(255, 8, 6, 5);

gp::Color withAlpha(gp::Color c, float a) {
    return gp::Color(static_cast<BYTE>(std::clamp(a, 0.0f, 1.0f) * c.GetA()), c.GetR(), c.GetG(), c.GetB());
}

const gp::FontFamily& serif() {
    static gp::FontFamily* fam = [] {
        for (const wchar_t* name : {L"Palatino Linotype", L"Book Antiqua", L"Georgia"}) {
            auto* f = new gp::FontFamily(name);
            if (f->GetLastStatus() == gp::Ok) return f;
            delete f;
        }
        return new gp::FontFamily(L"Times New Roman");
    }();
    return *fam;
}

// Small-caps label: upper case with hair spaces between the letters.
std::wstring spaced(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (!out.empty()) out += L' ';
        out += static_cast<wchar_t>(std::towupper(c));
    }
    return out;
}

std::wstring metres(float m) {
    wchar_t buf[32];
    swprintf(buf, 32, m < 10 ? L"%.1f m" : L"%.0f m", m);
    return buf;
}

gp::SizeF measure(gp::Graphics& g, const std::wstring& t, const gp::Font& f, float w,
                  const gp::StringFormat* fmt = nullptr) {
    if (t.empty()) return {0, 0};
    gp::RectF out;
    g.MeasureString(t.c_str(), -1, &f, gp::RectF(0, 0, w, 10000), fmt, &out);
    return {out.Width, out.Height};
}

// Text with a thin dark halo, so it reads over bright and dark scenes alike. Small text gets a
// lighter one, which would otherwise swallow its strokes.
void haloText(gp::Graphics& g, const std::wstring& t, const gp::Font& f, gp::Color c, gp::RectF r,
              const gp::StringFormat* fmt, float s) {
    if (t.empty()) return;
    const bool fine = f.GetSize() < 16 * s;
    gp::SolidBrush halo(gp::Color(static_cast<BYTE>(c.GetA() * (fine ? 0.4f : 0.6f)), 0, 0, 0));
    const float o = fine ? 1.0f : max(1.0f, 1.2f * s);
    const float offs[][2] = {{o, o}, {-o, 0}, {o, 0}, {0, -o}};
    for (const auto& d : offs)
        g.DrawString(t.c_str(), -1, &f, gp::RectF(r.X + d[0], r.Y + d[1], r.Width, r.Height), fmt, &halo);
    gp::SolidBrush b(c);
    g.DrawString(t.c_str(), -1, &f, r, fmt, &b);
}

// Soft shadow pooled behind some text, gone well before its edges: no visible box.
void shade(gp::Graphics& g, gp::RectF r, float strength) {
    gp::GraphicsPath path;
    path.AddEllipse(r);
    gp::PathGradientBrush b(&path);
    b.SetCenterColor(gp::Color(static_cast<BYTE>(255 * strength), 8, 6, 5));
    gp::Color edge(0, 8, 6, 5);
    int n = 1;
    b.SetSurroundColors(&edge, &n);
    b.SetFocusScales(0.35f, 0.2f);
    b.SetBlendBellShape(1.0f, 1.0f);  // a bell curve, so the edge never shows
    g.FillPath(&b, &path);
}

// A hairline that fades in from the left (and out at the right if both), optionally with a
// small diamond at its bright end.
void rule(gp::Graphics& g, float x0, float x1, float y, float s, gp::Color c, bool both, bool diamond) {
    gp::LinearGradientBrush b(gp::PointF(x0 - 1, y), gp::PointF(x1 + 1, y), c, c);
    gp::Color cols[] = {withAlpha(c, 0), c, both ? withAlpha(c, 0) : c};
    float pos[] = {0, both ? 0.5f : 1, 1};
    b.SetInterpolationColors(cols, pos, 3);
    gp::Pen pen(&b, max(1.0f, 1.0f * s));
    g.DrawLine(&pen, x0, y, x1, y);
    if (diamond) {
        float cx = both ? (x0 + x1) / 2 : x1, r = 3.5f * s;
        gp::PointF d[] = {{cx, y - r}, {cx + r, y}, {cx, y + r}, {cx - r, y}};
        gp::SolidBrush fill(c);
        g.FillPolygon(&fill, d, 4);
    }
}

// A diamond marker: soot outline, ember fill.
void diamond(gp::Graphics& g, float cx, float cy, float r, gp::Color c, float s) {
    gp::PointF d[] = {{cx, cy - r}, {cx + r * 0.72f, cy}, {cx, cy + r}, {cx - r * 0.72f, cy}};
    gp::SolidBrush fill(c);
    g.FillPolygon(&fill, d, 4);
    gp::Pen edge(gp::Color(200, 20, 12, 6), 1.5f * s);
    edge.SetLineJoin(gp::LineJoinMiter);
    g.DrawPolygon(&edge, d, 4);
}

// A bearing strip across the top: the diamond slides towards where you need to go.
void drawCompass(gp::Graphics& g, const OverlayView& v, float cx, float y, float s) {
    const float half = 130 * s;
    shade(g, gp::RectF(cx - half - 70 * s, y - 34 * s, 2 * half + 140 * s, 118 * s), 0.45f);

    rule(g, cx - half, cx + half, y, s, withAlpha(kEmber, 0.85f), true, false);
    gp::Pen tick(withAlpha(kAsh, 0.8f), max(1.0f, 1.0f * s));
    for (int k = -3; k <= 3; ++k) {
        float x = cx + k * half / 3, len = (k == 0 ? 7.0f : 3.5f) * s;
        if (std::abs(k) == 3) continue;
        g.DrawLine(&tick, x, y - len, x, y + (k == 0 ? len : 0));
    }

    if (v.arrived) {
        gp::Pen ring(kVerdant, 2 * s);
        g.DrawEllipse(&ring, cx - 7 * s, y - 7 * s, 14 * s, 14 * s);
    } else {
        // Straight ahead in the middle, 90 degrees either way at the ends.
        float t = std::clamp(v.relAngle / (kPi / 2), -1.0f, 1.0f);
        diamond(g, cx + t * half, y, 8 * s, kEmber, s);
        if (std::fabs(v.relAngle) > kPi / 2) {
            // Behind you: a chevron past the end of the strip, on the side to turn to.
            float dir = v.relAngle > 0 ? 1.0f : -1.0f, x = cx + dir * (half + 16 * s);
            gp::Pen chev(kEmber, 2 * s);
            chev.SetLineJoin(gp::LineJoinMiter);
            gp::PointF pts[] = {{x - dir * 5 * s, y - 7 * s}, {x + dir * 3 * s, y}, {x - dir * 5 * s, y + 7 * s}};
            g.DrawLines(&chev, pts, 3);
        }
    }

    gp::Font big(&serif(), 21 * s, gp::FontStyleRegular, gp::UnitPixel);
    gp::Font sub(&serif(), 14 * s, gp::FontStyleItalic, gp::UnitPixel);
    gp::StringFormat center;
    center.SetAlignment(gp::StringAlignmentCenter);
    std::wstring dist = v.arrived ? L"You have arrived" : metres(v.distance);
    std::wstring below;
    if (!v.arrived && std::fabs(v.heightDiff) >= 3)
        below = metres(std::fabs(v.heightDiff)) + (v.heightDiff > 0 ? L" above you" : L" below you");
    else if (!v.arrived && v.routeLeft > v.distance + 1)
        below = metres(v.routeLeft) + L" by the route";
    haloText(g, dist, big, v.arrived ? kVerdant : kEmber, gp::RectF(cx - 150 * s, y + 10 * s, 300 * s, 30 * s),
             &center, s);
    haloText(g, below, sub, kAsh, gp::RectF(cx - 150 * s, y + 37 * s, 300 * s, 22 * s), &center, s);
}

// The step, top right, right-aligned against the screen edge.
void drawPanel(gp::Graphics& g, const OverlayView& v, float right, float top, float s) {
    const gp::FontFamily& fam = serif();
    gp::Font label(&fam, 12.5f * s, gp::FontStyleRegular, gp::UnitPixel);
    gp::Font heading(&fam, 20 * s, gp::FontStyleRegular, gp::UnitPixel);
    gp::Font body(&fam, 19 * s, gp::FontStyleRegular, gp::UnitPixel);
    gp::Font warn(&fam, 15 * s, gp::FontStyleItalic, gp::UnitPixel);
    gp::Font note(&fam, 14 * s, gp::FontStyleItalic, gp::UnitPixel);
    gp::Font keys(&fam, 12 * s, gp::FontStyleRegular, gp::UnitPixel);
    gp::StringFormat toRight;
    toRight.SetAlignment(gp::StringAlignmentFar);
    const float W = 430 * s, x = right - W;

    // "Northern Undead Asylum (7/10)" becomes the area as a label plus its count; any other
    // title ("Buy from Andre") is a heading under a "Step 512 / 654" label.
    std::wstring labelText, headingText, count;
    size_t open = v.title.rfind(L" (");
    int num = 0, den = 0;
    wchar_t end = 0;
    if (open != std::wstring::npos && swscanf(v.title.c_str() + open, L" (%d/%d%lc", &num, &den, &end) == 3 &&
        end == L')') {
        labelText = spaced(v.title.substr(0, open));
        count = v.title.substr(open + 2, v.title.size() - open - 3);
    } else {
        labelText = spaced(v.header);
        headingText = v.title;
    }
    if (!count.empty()) labelText += L" " + count;

    // Status ("Do the thing here...") and the key line both arrive in `hint`.
    std::wstring status, keyLine = v.hint;
    size_t nl = v.hint.rfind(L'\n');
    if (nl != std::wstring::npos) {
        status = v.hint.substr(0, nl);
        keyLine = v.hint.substr(nl + 1);
    }
    if (!count.empty()) keyLine = v.header + L"  ·  " + keyLine;
    std::wstring noteText = v.progress.empty() ? status
                          : status.empty()     ? v.progress
                                               : v.progress + L" — " + status;
    if (!noteText.empty()) noteText[0] = static_cast<wchar_t>(std::towupper(noteText[0]));

    struct Line {
        const std::wstring* text;
        const gp::Font* font;
        gp::Color color;
        float gapAfter;
    };
    // Calm (panelStrength towards 0) dims the area and instruction and fades out the hints; a
    // warning stays clear. The layout doesn't move, so nothing jumps when it comes back.
    const float f = v.panelStrength, dim = 0.45f + 0.55f * f;
    Line lines[] = {{&headingText, &heading, withAlpha(kEmber, dim), 4 * s},
                    {&v.body, &body, withAlpha(kBone, dim), 8 * s},
                    {&v.warning, &warn, withAlpha(kBlood, 0.75f + 0.25f * f), 8 * s},
                    {&noteText, &note, withAlpha(kAsh, f), 6 * s},
                    {&keyLine, &keys, withAlpha(kAsh, 0.75f * f), 0}};
    float labelH = measure(g, labelText, label, W, &toRight).Height;
    float h = labelH + 12 * s;
    float heights[5];
    for (int i = 0; i < 5; ++i) {
        heights[i] = measure(g, *lines[i].text, *lines[i].font, W, &toRight).Height;
        if (heights[i] > 0) h += heights[i] + lines[i].gapAfter;
    }

    shade(g, gp::RectF(x - 90 * s, top - 50 * s, W + 260 * s, h + 100 * s), 0.55f * (0.5f + 0.5f * f));
    haloText(g, labelText, label, withAlpha(kAsh, dim), gp::RectF(x, top, W, labelH + 2), &toRight, s);
    float y = top + labelH + 4 * s;
    rule(g, right - 240 * s, right, y, s, withAlpha(kEmber, 0.8f * dim), false, true);
    y += 8 * s;
    for (int i = 0; i < 5; ++i) {
        if (heights[i] <= 0) continue;
        if (lines[i].color.GetA() < 4) {  // faded out entirely
            y += heights[i] + lines[i].gapAfter;
            continue;
        }
        haloText(g, *lines[i].text, *lines[i].font, lines[i].color, gp::RectF(x, y, W, heights[i] + 4), &toRight, s);
        y += heights[i] + lines[i].gapAfter;
    }

    if (!v.debug.empty()) {
        gp::FontFamily mono(L"Consolas");
        gp::Font dbg(&mono, 12 * s, gp::FontStyleRegular, gp::UnitPixel);
        std::wstring all;
        for (auto& line : v.debug) all += line + L"\n";
        gp::SizeF sz = measure(g, all, dbg, W);
        y += 14 * s;
        gp::SolidBrush back(gp::Color(170, 8, 6, 5));
        g.FillRectangle(&back, right - sz.Width - 16 * s, y, sz.Width + 16 * s, sz.Height + 12 * s);
        gp::SolidBrush b(kBone);
        g.DrawString(all.c_str(), -1, &dbg, gp::PointF(right - sz.Width - 8 * s, y + 6 * s), &b);
    }
}

// The part of the window the game renders into: its projection's aspect, centred (letterboxing).
struct Viewport {
    float cx, cy, hw, hh;  // centre and half size, pixels
};

Viewport fitViewport(int w, int h, float aspect) {
    float vw = static_cast<float>(w), vh = static_cast<float>(h);
    if (vw / vh > aspect) vw = vh * aspect;
    else vh = vw / aspect;
    return {w / 2.0f, h / 2.0f, vw / 2, vh / 2};
}

const float kNear = 0.4f;

gp::PointF toScreen(const Camera& c, const Viewport& vp, const Vec3& v) {  // v.z >= kNear
    return {vp.cx + vp.hw * c.xs * v.x / v.z, vp.cy - vp.hh * c.ys * v.y / v.z};
}

// Clips segment a-b to the rectangle; false if nothing is left (Liang-Barsky).
bool clip2d(gp::PointF& a, gp::PointF& b, float x0, float y0, float x1, float y1) {
    float t0 = 0, t1 = 1, dx = b.X - a.X, dy = b.Y - a.Y;
    float p[] = {-dx, dx, -dy, dy}, q[] = {a.X - x0, x1 - a.X, a.Y - y0, y1 - a.Y};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        float t = q[i] / p[i];
        if (p[i] < 0) t0 = max(t0, t);
        else t1 = min(t1, t);
        if (t0 > t1) return false;
    }
    gp::PointF a2{a.X + t0 * dx, a.Y + t0 * dy};
    b = {a.X + t1 * dx, a.Y + t1 * dy};
    a = a2;
    return true;
}

void drawTrail(gp::Graphics& g, const OverlayView& v, int w, int h, float s) {
    const Camera& c = *v.camera;
    Viewport vp = fitViewport(w, h, c.aspect());
    const float focal = vp.hh * c.ys;  // pixels per metre at 1 m depth
    const Vec3 lift{0, 0.12f, 0};      // just above the ground so it isn't lost in the floor

    struct Piece {
        gp::PointF a, b;
        float width, alpha;
        bool mark;
    };
    std::vector<Piece> pieces;
    float along = 0, nextMark = 4;
    for (size_t i = 0; i + 1 < v.trail.size(); ++i) {
        Vec3 A = v.trail[i] + lift, B = v.trail[i + 1] + lift;
        float len = length(B - A);
        int steps = max(1, static_cast<int>(std::ceil(len / 1.5f)));
        for (int k = 0; k < steps; ++k) {
            float t0 = static_cast<float>(k) / steps, t1 = static_cast<float>(k + 1) / steps;
            float a0 = along + len * t0, a1 = along + len * t1;
            if (a1 < 1.2f) continue;  // under the character
            Vec3 v0 = c.toView(A + (B - A) * t0), v1 = c.toView(A + (B - A) * t1);
            if (v0.z < kNear && v1.z < kNear) continue;
            if (v0.z < kNear) v0 = v0 + (v1 - v0) * ((kNear - v0.z) / (v1.z - v0.z));
            if (v1.z < kNear) v1 = v1 + (v0 - v1) * ((kNear - v1.z) / (v0.z - v1.z));
            gp::PointF p0 = toScreen(c, vp, v0), p1 = toScreen(c, vp, v1);
            if (!clip2d(p0, p1, -50.0f, -50.0f, w + 50.0f, h + 50.0f)) continue;

            float depth = (v0.z + v1.z) / 2;
            // Calm makes it thinner as well as fainter.
            float width = std::clamp(0.2f * focal / depth, 2 * s, 8 * s) * (0.7f + 0.3f * v.trailStrength);
            // Clear for the next 20 m, then fading out so it doesn't scribble over the whole view
            // (the beacon still marks the destination); faded in at your feet.
            float mid = (a0 + a1) / 2;
            float alpha = mid < 20 ? 1 : max(0.1f, 1 - (mid - 20) / 60);
            alpha *= std::clamp((mid - 1.2f) / 1.8f, 0.0f, 1.0f) * v.trailStrength;
            bool mark = a1 >= nextMark;
            if (mark) nextMark += 4;
            pieces.push_back({p0, p1, width, alpha, mark});
        }
        along += len;
    }

    auto pass = [&](gp::Color col, float widthScale, float extra) {
        for (const Piece& p : pieces) {
            gp::Pen pen(withAlpha(col, p.alpha), p.width * widthScale + extra);
            pen.SetStartCap(gp::LineCapRound);
            pen.SetEndCap(gp::LineCapRound);
            g.DrawLine(&pen, p.a, p.b);
        }
    };
    pass(gp::Color(40, 230, 176, 98), 3.2f, 0);    // faint ember glow
    pass(gp::Color(120, 20, 10, 4), 1, 2.5f * s);   // dark edge, for bright floors
    pass(gp::Color(240, 246, 214, 150), 1, 0);      // pale core

    // Small soot chevrons along the core say which way the trail runs.
    for (const Piece& p : pieces) {
        if (!p.mark) continue;
        float dx = p.b.X - p.a.X, dy = p.b.Y - p.a.Y, d = std::hypot(dx, dy);
        if (d < 1) continue;
        dx /= d;
        dy /= d;
        float sz = p.width * 0.75f;
        gp::PointF m{(p.a.X + p.b.X) / 2 + dx * sz * 0.5f, (p.a.Y + p.b.Y) / 2 + dy * sz * 0.5f};
        gp::PointF pts[] = {{m.X - dx * sz - dy * sz, m.Y - dy * sz + dx * sz},
                            m,
                            {m.X - dx * sz + dy * sz, m.Y - dy * sz - dx * sz}};
        gp::Pen pen(gp::Color(static_cast<BYTE>(220 * p.alpha), 70, 38, 12), max(1.4f, p.width * 0.32f));
        pen.SetLineJoin(gp::LineJoinMiter);
        g.DrawLines(&pen, pts, 3);
    }
}

// How strongly the motes show `d` metres from your feet: clear for the next 20 m, then fading
// out; faded in at your feet so nothing sits on the character.
float moteFade(float d, float strength) {
    float a = d < 20 ? 1 : max(0.0f, 1 - (d - 20) / 50);
    return a * std::clamp((d - 1.2f) / 1.8f, 0.0f, 1.0f) * strength;
}

// Steady per-mote randomness in [0, 1): mote k, channel c. Each mote keeps its own size,
// flicker and offset from frame to frame.
float noise(int k, int c) {
    uint32_t x = static_cast<uint32_t>(k) * 0x9E3779B1u ^ static_cast<uint32_t>(c + 1) * 0x85EBCA77u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    x *= 0x297A2D39u;
    x ^= x >> 15;
    return (x & 0xFFFFFF) / 16777216.0f;
}

// One light: where it is, how bright and big, and an optional tail (a world offset behind it).
struct Mote {
    Vec3 pos;
    float bright, size;
    Vec3 tail;
    bool hasTail;
    float depth;
};

// The trail as motes: small soft lights placed along the route, each style animating them its own
// way. They're pinned to the route (see trailBase), so they don't slide along with you as you walk.
void drawMotes(gp::Graphics& g, const OverlayView& v, int w, int h, float s) {
    const Camera& c = *v.camera;
    Viewport vp = fitViewport(w, h, c.aspect());
    const float focal = vp.hh * c.ys, margin = 40 * s, t = v.timeSec;
    const float pi2 = 2 * kPi;

    // Distance along the step's route of each trail point (the first is your feet).
    std::vector<float> at(v.trail.size());
    float lead = length(v.trail.size() > 1 ? v.trail[1] - v.trail[0] : Vec3{});
    at[0] = v.trailBase - lead;
    for (size_t i = 1; i < v.trail.size(); ++i)
        at[i] = i == 1 ? v.trailBase : at[i - 1] + length(v.trail[i] - v.trail[i - 1]);
    const float feet = at.front(), end = at.back();
    // The route's point and heading `m` metres along it.
    size_t seg = 0;
    auto sample = [&](float m, Vec3& pos, Vec3& dir) {
        if (m < at.front() || m > at.back()) return false;
        while (seg > 0 && at[seg] > m) --seg;
        while (seg + 2 < at.size() && at[seg + 1] < m) ++seg;
        float len = at[seg + 1] - at[seg];
        Vec3 d = v.trail[seg + 1] - v.trail[seg];
        pos = v.trail[seg] + d * (len > 0 ? (m - at[seg]) / len : 0);
        float dl = length(d);
        dir = dl > 0 ? d * (1 / dl) : Vec3{0, 0, 1};
        return true;
    };

    std::vector<Mote> motes;
    auto add = [&](float m, Vec3 offset, float bright, float size, float tailLen) {
        Vec3 pos, dir;
        float fade = moteFade(m - feet, v.trailStrength);
        if (fade * bright < 0.02f || !sample(m, pos, dir)) return;
        Vec3 side{dir.z, 0, -dir.x};  // across the path, level
        float sl = length(side);
        side = sl > 0 ? side * (1 / sl) : Vec3{1, 0, 0};
        Vec3 p = pos + side * offset.x + Vec3{0, offset.y, 0} + dir * offset.z;
        Vec3 view = c.toView(p);
        if (view.z < kNear) return;
        motes.push_back({p, bright * fade, size, dir * -tailLen + Vec3{0, -0.04f * tailLen, 0}, tailLen > 0, view.z});
    };
    // Motes k = first..last at k * spacing + flow metres along the route.
    auto each = [&](float spacing, float flow, auto&& fn) {
        int first = static_cast<int>(std::ceil((feet - flow) / spacing));
        int last = static_cast<int>(std::floor((end - flow) / spacing));
        for (int k = first; k <= last && k - first < 600; ++k) fn(k, k * spacing + flow);
    };

    switch (v.moteStyle) {
        case kMoteEmbers:
            // Embers drifting forward: scattered around the path, each its own size and flicker,
            // with a short glowing tail.
            each(1.3f, t * 1.0f, [&](int k, float m) {
                float flick = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(t * (2.5f + 2 * noise(k, 3)) + pi2 * noise(k, 4)));
                Vec3 off{(noise(k, 1) - 0.5f) * 0.5f, 0.15f + noise(k, 2) * 0.35f + 0.06f * std::sin(t * 1.3f + k), 0};
                add(m, off, flick, 0.6f + 0.8f * noise(k, 5), 0.45f);
            });
            break;
        case kMotePulse:
            // Still motes with soft bands of light travelling through them towards the destination.
            each(0.9f, 0, [&](int, float m) {
                float wave = 0.5f + 0.5f * std::cos(pi2 * (m - t * 5.5f) / 7.0f);
                float crest = wave * wave * wave * wave * wave * wave;
                add(m, Vec3{0, 0.25f, 0}, 0.18f + 0.82f * crest, 0.7f + 0.5f * crest, 0);
            });
            break;
        case kMoteWisps:
            // Two strands of tiny motes spiralling around the path as they flow along it.
            each(0.55f, t * 1.3f, [&](int k, float m) {
                float a = m * 2.2f + (k & 1) * kPi + t * 0.5f;
                Vec3 off{std::cos(a) * 0.22f, 0.35f + std::sin(a) * 0.22f, 0};
                add(m, off, 0.7f + 0.3f * std::sin(a), 0.55f, 0);
            });
            break;
        default:  // kMoteFireflies
            // Sparse lights that fade in, wander gently, drift forward and fade out again.
            each(2.2f, t * 0.5f, [&](int k, float m) {
                float period = 3 + 2 * noise(k, 2);
                float u = std::fmod(t / period + noise(k, 3), 1.0f);
                float life = std::sin(kPi * u);
                Vec3 off{0.5f * std::sin(t * 0.6f * (1 + noise(k, 4)) + pi2 * noise(k, 5)),
                         0.3f + 0.35f * (0.5f + 0.5f * std::sin(t * 0.8f + k)), noise(k, 1) - 0.5f};
                add(m, off, life * life, 0.8f + 0.6f * noise(k, 6), 0);
            });
            break;
    }

    // Far ones first, so near motes sit on top.
    std::sort(motes.begin(), motes.end(), [](const Mote& a, const Mote& b) { return a.depth > b.depth; });
    for (const Mote& m : motes) {
        gp::PointF sp = toScreen(c, vp, c.toView(m.pos));
        if (sp.X < -margin || sp.X > w + margin || sp.Y < -margin || sp.Y > h + margin) continue;
        float r = std::clamp(0.05f * focal / m.depth * m.size, 1.0f * s, 5.0f * s);
        float a = std::clamp(m.bright, 0.0f, 1.0f);
        if (m.hasTail) {
            Vec3 tv = c.toView(m.pos + m.tail);
            if (tv.z >= kNear) {
                gp::PointF tp = toScreen(c, vp, tv);
                if (std::hypot(tp.X - sp.X, tp.Y - sp.Y) > 1) {
                    gp::LinearGradientBrush streak(tp, sp, gp::Color(0, 240, 196, 120),
                                                   gp::Color(static_cast<BYTE>(120 * a), 246, 210, 140));
                    gp::Pen pen(&streak, r * 1.1f);
                    pen.SetStartCap(gp::LineCapRound);
                    pen.SetEndCap(gp::LineCapRound);
                    g.DrawLine(&pen, tp, sp);
                }
            }
        }
        gp::SolidBrush halo(gp::Color(static_cast<BYTE>(34 * a), 230, 170, 92)),
            glow(gp::Color(static_cast<BYTE>(80 * a), 240, 194, 118)),
            core(gp::Color(static_cast<BYTE>(235 * a), 253, 236, 192));
        g.FillEllipse(&halo, sp.X - r * 3.2f, sp.Y - r * 3.2f, r * 6.4f, r * 6.4f);
        g.FillEllipse(&glow, sp.X - r * 1.8f, sp.Y - r * 1.8f, r * 3.6f, r * 3.6f);
        g.FillEllipse(&core, sp.X - r, sp.Y - r, r * 2, r * 2);
    }
}

// A beam over the destination with a diamond on top, or a chevron at the screen edge.
void drawGoalMarker(gp::Graphics& g, const OverlayView& v, int w, int h, float s) {
    const Camera& c = *v.camera;
    Viewport vp = fitViewport(w, h, c.aspect());
    const Vec3 goal = v.trail.back();
    Vec3 vb = c.toView(goal), vt = c.toView(goal + Vec3{0, 2.4f, 0});

    gp::Font big(&serif(), 17 * s, gp::FontStyleRegular, gp::UnitPixel);
    gp::Font sub(&serif(), 13 * s, gp::FontStyleItalic, gp::UnitPixel);
    gp::StringFormat center;
    center.SetAlignment(gp::StringAlignmentCenter);
    std::wstring dist = metres(v.goalLeft), height;
    if (std::fabs(v.goalHeight) >= 3)
        height = metres(std::fabs(v.goalHeight)) + (v.goalHeight > 0 ? L" up" : L" down");
    // Softer while calm, but never so faint you lose where you're going.
    const float a = 0.55f + 0.45f * v.trailStrength;
    const gp::Color ember = withAlpha(kEmber, a);
    // Distance, and the climb under it, centred on (cx, y).
    auto drawLabel = [&](float cx, float y) {
        haloText(g, dist, big, ember, gp::RectF(cx - 90 * s, y, 180 * s, 24 * s), &center, s);
        haloText(g, height, sub, withAlpha(kAsh, a), gp::RectF(cx - 90 * s, y + 21 * s, 180 * s, 20 * s),
                 &center, s);
    };
    const float labelH = (height.empty() ? 24 : 42) * s;

    const float inset = 24 * s;
    bool onScreen = false;
    gp::PointF base, top;
    if (vb.z >= kNear && vt.z >= kNear) {
        base = toScreen(c, vp, vb);
        top = toScreen(c, vp, vt);
        onScreen = base.X > inset && base.X < w - inset && top.Y > inset && base.Y < h + 200 * s;
    }
    if (onScreen) {
        float focal = vp.hh * c.ys;
        float beamW = std::clamp(0.3f * focal / vb.z, 1.5f * s, 6 * s);
        gp::LinearGradientBrush beam(gp::PointF(base.X, base.Y + 1), gp::PointF(top.X, top.Y),
                                     gp::Color(static_cast<BYTE>(200 * a), 246, 214, 150),
                                     gp::Color(0, 246, 214, 150));
        gp::Pen pen(&beam, beamW);
        g.DrawLine(&pen, base, top);
        float r = std::clamp(0.4f * focal / vb.z, 7 * s, 13 * s);
        diamond(g, top.X, top.Y, r, ember, s);
        drawLabel(top.X, top.Y - r - labelH - 6 * s);
        return;
    }

    // Off screen: point along the screen-space direction; behind you leans to the lower edge.
    Vec3 vm = c.toView(goal + Vec3{0, 1, 0});
    float dx = vm.x, dy = -vm.y;
    if (vm.z < kNear) {
        dx = vm.x >= 0 ? 1.0f : -1.0f;
        dy = 0.35f;
    }
    float rx = vp.hw - 70 * s, ry = vp.hh - 70 * s;
    float k = 1 / std::sqrt((dx / rx) * (dx / rx) + (dy / ry) * (dy / ry));
    float px = vp.cx + dx * k, py = vp.cy + dy * k;
    float ang = std::atan2(dy, dx) * 180 / kPi;
    gp::Matrix m;
    m.Translate(px, py);
    m.Rotate(ang);
    g.SetTransform(&m);
    const float R = 16 * s;
    gp::PointF chev[] = {{-R * 0.6f, -R}, {R * 0.4f, 0}, {-R * 0.6f, R}};
    gp::Pen halo(gp::Color(static_cast<BYTE>(150 * a), 0, 0, 0), 6 * s), pen(ember, 3 * s);
    halo.SetLineJoin(gp::LineJoinMiter);
    pen.SetLineJoin(gp::LineJoinMiter);
    g.DrawLines(&halo, chev, 3);
    g.DrawLines(&pen, chev, 3);
    g.ResetTransform();
    // Label on the inner side of the chevron, clear of it whichever way it points.
    float ux = std::cos(ang * kPi / 180), uy = std::sin(ang * kPi / 180);
    float back = R + 12 * s + std::fabs(ux) * 50 * s + std::fabs(uy) * labelH / 2;
    drawLabel(px - ux * back, py - uy * back - labelH / 2);
}

// A short message across the screen on a band of shadow that fades out at both ends.
void drawNotice(gp::Graphics& g, const std::wstring& text, float cx, float y, float s) {
    gp::Font f(&serif(), 21 * s, gp::FontStyleRegular, gp::UnitPixel);
    std::wstring t = spaced(text);
    gp::SizeF sz = measure(g, t, f, 2000 * s);
    float half = sz.Width / 2 + 140 * s, bandH = sz.Height + 22 * s;
    gp::LinearGradientBrush band(gp::PointF(cx - half - 1, y), gp::PointF(cx + half + 1, y), kSoot, kSoot);
    gp::Color cols[] = {gp::Color(0, 8, 6, 5), gp::Color(185, 8, 6, 5), gp::Color(0, 8, 6, 5)};
    float pos[] = {0, 0.5f, 1};
    band.SetInterpolationColors(cols, pos, 3);
    g.FillRectangle(&band, cx - half, y, 2 * half, bandH);
    rule(g, cx - half * 0.8f, cx + half * 0.8f, y, s, withAlpha(kEmber, 0.6f), true, false);
    rule(g, cx - half * 0.8f, cx + half * 0.8f, y + bandH, s, withAlpha(kEmber, 0.6f), true, false);
    gp::StringFormat center;
    center.SetAlignment(gp::StringAlignmentCenter);
    haloText(g, t, f, kEmber, gp::RectF(cx - half, y + 11 * s, 2 * half, sz.Height + 4), &center, s);
}
}  // namespace

void paintOverlay(gp::Graphics& g, int w, int h, const OverlayView& v) {
    float s = std::clamp(h / 1080.0f, 0.6f, 2.5f);
    if (v.camera && v.trail.size() >= 2) {
        if (v.particles) drawMotes(g, v, w, h, s);
        else drawTrail(g, v, w, h, s);
        if (v.trailEndsAtGoal) drawGoalMarker(g, v, w, h, s);
    }
    if (v.showCompass) drawCompass(g, v, w / 2.0f, 46 * s, s);
    // Top-right: the game's own HP/stamina bars live top-left, souls bottom-right.
    drawPanel(g, v, w - 40 * s, 34 * s, s);
    if (!v.notice.empty()) drawNotice(g, v.notice, w / 2.0f, h * 0.7f, s);
}

bool Overlay::create(HINSTANCE inst) {
    gp::GdiplusStartupInput in;
    if (gp::GdiplusStartup(&gdiplusToken_, &in, nullptr) != gp::Ok) return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClass, L"DSR Pathfinder", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    return hwnd_ != nullptr;
}

Overlay::~Overlay() {
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
    }
    if (dib_) DeleteObject(dib_);
    if (hwnd_) DestroyWindow(hwnd_);
    if (gdiplusToken_) gp::GdiplusShutdown(gdiplusToken_);
}

void Overlay::hide() {
    if (visible_) ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

void Overlay::ensureSurface(int w, int h) {
    if (w == w_ && h == h_ && dib_) return;
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
    }
    if (dib_) DeleteObject(dib_);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    oldBmp_ = SelectObject(memDC_, dib_);
    w_ = w;
    h_ = h;
}

void Overlay::render(const RECT& rc, const OverlayView& v) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return hide();
    ensureSurface(w, h);

    {
        // Draw straight into the DIB as premultiplied ARGB, which UpdateLayeredWindow expects.
        gp::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits_));
        gp::Graphics g(&bmp);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        g.Clear(gp::Color(0, 0, 0, 0));
        paintOverlay(g, w, h, v);
    }

    POINT dst{rc.left, rc.top}, src{0, 0};
    SIZE size{w, h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &dst, &size, memDC_, &src, 0, &blend, ULW_ALPHA);
    if (!visible_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        visible_ = true;
    }
}

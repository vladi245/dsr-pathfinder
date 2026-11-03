#include "route.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <queue>

#include "camera.h"  // Vec3 operators

namespace {
constexpr float kCell = 4;
constexpr size_t kMaxExpanded = 400000;

int64_t cellKey(int gx, int gz) { return (static_cast<int64_t>(gx) << 32) ^ static_cast<uint32_t>(gz); }
int cellOf(float v) { return static_cast<int>(std::floor(v / kCell)); }
float flatDist(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.z - b.z); }
}  // namespace

bool NavMesh::load(const std::filesystem::path& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::vector<char> data;
    char buf[1 << 16];
    for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + got);
    std::fclose(f);
    return load(std::string_view(data.data(), data.size()));
}

bool NavMesh::load(std::string_view data) {
    size_t at = 0;
    auto readAll = [&](void* dst, size_t n) {
        if (data.size() - at < n) return false;
        std::memcpy(dst, data.data() + at, n);
        at += n;
        return true;
    };
    char magic[4];
    uint32_t version = 0, n = 0, total = 0;
    bool ok = readAll(magic, 4) && std::memcmp(magic, "DSRN", 4) == 0 && readAll(&version, 4) &&
              version == 1 && readAll(&n, 4) && readAll(&total, 4);
    if (ok) {
        std::vector<float> corners(static_cast<size_t>(n) * 9);
        scrap_.resize(n);
        first_.resize(static_cast<size_t>(n) + 1);
        ok = readAll(corners.data(), corners.size() * 4) && readAll(scrap_.data(), n) &&
             readAll(first_.data(), first_.size() * 4);
        if (ok) {
            tri_.resize(n);
            centre_.resize(n);
            for (uint32_t t = 0; t < n; ++t) {
                const float* c = &corners[t * 9];
                tri_[t] = {Vec3{c[0], c[1], c[2]}, Vec3{c[3], c[4], c[5]}, Vec3{c[6], c[7], c[8]}};
                centre_[t] = (tri_[t][0] + tri_[t][1] + tri_[t][2]) * (1.0f / 3);
            }
            links_.resize(total);
            for (uint32_t i = 0; ok && i < total; ++i) {
                uint8_t raw[36];
                ok = readAll(raw, sizeof raw);
                Link& l = links_[i];
                std::memcpy(&l.to, raw, 4);
                std::memcpy(&l.cost, raw + 4, 4);
                l.kind = raw[8];
                std::memcpy(&l.a, raw + 12, 12);
                std::memcpy(&l.b, raw + 24, 12);
            }
        }
    }
    if (!ok || first_.back() != total) {
        tri_.clear();
        return false;
    }
    for (uint32_t t = 0; t < tri_.size(); ++t) {
        float x0 = std::min({tri_[t][0].x, tri_[t][1].x, tri_[t][2].x});
        float x1 = std::max({tri_[t][0].x, tri_[t][1].x, tri_[t][2].x});
        float z0 = std::min({tri_[t][0].z, tri_[t][1].z, tri_[t][2].z});
        float z1 = std::max({tri_[t][0].z, tri_[t][1].z, tri_[t][2].z});
        for (int gx = cellOf(x0); gx <= cellOf(x1); ++gx)
            for (int gz = cellOf(z0); gz <= cellOf(z1); ++gz) grid_[cellKey(gx, gz)].push_back(static_cast<int>(t));
    }
    return true;
}

bool NavMesh::contains(int t, float x, float z) const {
    const Vec3 &a = tri_[t][0], &b = tri_[t][1], &c = tri_[t][2];
    float d1 = (x - b.x) * (a.z - b.z) - (a.x - b.x) * (z - b.z);
    float d2 = (x - c.x) * (b.z - c.z) - (b.x - c.x) * (z - c.z);
    float d3 = (x - a.x) * (c.z - a.z) - (c.x - a.x) * (z - a.z);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

float NavMesh::height(int t, float x, float z) const {
    const Vec3 &a = tri_[t][0], &b = tri_[t][1], &c = tri_[t][2];
    float det = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::fabs(det) < 1e-9f) return centre_[t].y;
    float w1 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / det;
    float w2 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / det;
    return w1 * a.y + w2 * b.y + (1 - w1 - w2) * c.y;
}

std::vector<int> NavMesh::under(float x, float z) const {
    std::vector<int> out;
    auto it = grid_.find(cellKey(cellOf(x), cellOf(z)));
    if (it == grid_.end()) return out;
    for (int t : it->second)
        if (!scrap_[t] && contains(t, x, z)) out.push_back(t);
    return out;
}

int NavMesh::surfaceAt(const Vec3& p, float maxRise) const {
    int best = -1;
    float bestDy = 0;
    for (int t : under(p.x, p.z)) {
        float dy = std::fabs(height(t, p.x, p.z) - p.y);
        if (dy <= maxRise && (best < 0 || dy < bestDy)) {
            best = t;
            bestDy = dy;
        }
    }
    return best;
}

float NavMesh::distanceTo(int t, const Vec3& p) const {
    float flat = 0;
    if (!contains(t, p.x, p.z)) {
        flat = std::numeric_limits<float>::infinity();
        for (int k = 0; k < 3; ++k) {
            const Vec3 &a = tri_[t][k], &b = tri_[t][(k + 1) % 3];
            float ex = b.x - a.x, ez = b.z - a.z, len2 = ex * ex + ez * ez;
            float f = len2 > 0 ? std::clamp(((p.x - a.x) * ex + (p.z - a.z) * ez) / len2, 0.0f, 1.0f) : 0;
            flat = std::min(flat, std::hypot(p.x - (a.x + ex * f), p.z - (a.z + ez * f)));
        }
    }
    float lo = std::min({tri_[t][0].y, tri_[t][1].y, tri_[t][2].y});
    float hi = std::max({tri_[t][0].y, tri_[t][1].y, tri_[t][2].y});
    float rise = std::max({0.0f, lo - p.y, p.y - hi});
    return std::hypot(flat, rise);
}

int NavMesh::nearest(const Vec3& p, float radius) const {
    int t = surfaceAt(p, 3);
    if (t >= 0) return t;
    int best = -1;
    float bestD = radius;
    int r = static_cast<int>(radius / kCell) + 1, gx = cellOf(p.x), gz = cellOf(p.z);
    for (int dx = -r; dx <= r; ++dx)
        for (int dz = -r; dz <= r; ++dz) {
            auto it = grid_.find(cellKey(gx + dx, gz + dz));
            if (it == grid_.end()) continue;
            for (int c : it->second) {
                if (scrap_[c]) continue;
                float d = distanceTo(c, p);
                if (d < bestD) {
                    best = c;
                    bestD = d;
                }
            }
        }
    return best;
}

bool NavMesh::neighbours(int a, int b) const {
    for (uint32_t i = first_[a]; i < first_[a + 1]; ++i)
        if (static_cast<int>(links_[i].to) == b && (links_[i].kind == kWalk || links_[i].kind == kSeam)) return true;
    return false;
}

// Walking straight from p to q stays on one connected stretch of the corridor's floor: every
// sample near the line's height, on the same triangle as the last or a neighbour of it. Without
// the neighbour rule a line through a stairwell passes, because some floor is always near it.
bool NavMesh::walkable(const Vec3& p, const Vec3& q, const std::vector<uint8_t>& allowed) const {
    int n = std::max(2, static_cast<int>(length(q - p) / 0.5f));
    int cur = -1;
    for (int k = 0; k <= n; ++k) {
        Vec3 s = p + (q - p) * (static_cast<float>(k) / n);
        std::vector<int> here;
        for (int t : under(s.x, s.z))
            if (allowed[t] && std::fabs(height(t, s.x, s.z) - s.y) <= 1.0f) here.push_back(t);
        if (here.empty()) {
            if (k == 0 || k == n) continue;  // the ends sit on portal edges, between triangles
            return false;
        }
        if (std::find(here.begin(), here.end(), cur) != here.end()) continue;
        if (cur < 0) {
            cur = *std::min_element(here.begin(), here.end(), [&](int a, int b) {
                return std::fabs(height(a, s.x, s.z) - s.y) < std::fabs(height(b, s.x, s.z) - s.y);
            });
            continue;
        }
        auto next = std::find_if(here.begin(), here.end(), [&](int t) { return neighbours(cur, t); });
        if (next == here.end()) return false;
        cur = *next;
    }
    return true;
}

bool NavMesh::route(const Vec3& start, const Vec3& goal, std::vector<Vec3>& out) const {
    out.clear();
    if (!loaded()) return false;
    // A boss on a ledge or an item over water can be well off the floor: start from the nearest.
    int a = nearest(start, 8), b = nearest(goal, 8);
    if (a < 0) a = nearest(start, 25);
    if (b < 0) b = nearest(goal, 25);
    if (a < 0 || b < 0) return false;
    if (a == b) {
        out = {start, goal};
        return true;
    }

    const size_t n = tri_.size();
    std::vector<float> cost(n, std::numeric_limits<float>::infinity());
    std::vector<int> cameFrom(n, -1), viaLink(n, -1);
    struct Open {
        float f, g;
        int t;
        bool operator<(const Open& o) const { return f > o.f; }
    };
    std::priority_queue<Open> open;
    const Vec3 target = centre_[b];
    cost[a] = 0;
    open.push({length(centre_[a] - target), 0, a});
    int closest = a;
    float closestD = length(centre_[a] - target);
    size_t expanded = 0;
    bool complete = false;
    while (!open.empty() && expanded < kMaxExpanded) {
        Open o = open.top();
        open.pop();
        if (o.t == b) {
            complete = true;
            break;
        }
        if (o.g > cost[o.t]) continue;
        ++expanded;
        float d = length(centre_[o.t] - target);
        if (d < closestD) {
            closest = o.t;
            closestD = d;
        }
        for (uint32_t i = first_[o.t]; i < first_[o.t + 1]; ++i) {
            const Link& l = links_[i];
            float g = o.g + l.cost;
            if (g < cost[l.to]) {
                cost[l.to] = g;
                cameFrom[l.to] = o.t;
                viaLink[l.to] = static_cast<int>(i);
                open.push({g + length(centre_[l.to] - target), g, static_cast<int>(l.to)});
            }
        }
    }
    int end = complete ? b : closest;
    if (end == a) {
        out = {start, goal};
        return true;
    }

    // Corridor of triangles, and the portal crossed into each, from start to the end.
    std::vector<int> corridor;
    std::vector<const Link*> steps;
    for (int t = end; t != a; t = cameFrom[t]) {
        corridor.push_back(t);
        steps.push_back(&links_[viaLink[t]]);
    }
    corridor.push_back(a);
    std::reverse(corridor.begin(), corridor.end());
    std::reverse(steps.begin(), steps.end());
    std::vector<uint8_t> allowed(n, 0);
    for (int t : corridor) allowed[t] = 1;

    // Portal midpoints, then drop every point that a straight walk can skip. Gaps and drops stay.
    std::vector<Vec3> pts{start};
    std::vector<bool> keep{true};
    for (const Link* l : steps) {
        pts.push_back((l->a + l->b) * 0.5f);
        keep.push_back(l->kind == kGap || l->kind == kDrop);
    }
    pts.push_back(complete ? goal : centre_[end]);
    keep.push_back(true);
    out.push_back(pts[0]);
    for (size_t i = 0; i + 1 < pts.size();) {
        size_t j = i + 1;
        while (j + 1 < pts.size() && !keep[j] && walkable(pts[i], pts[j + 1], allowed)) ++j;
        out.push_back(pts[j]);
        i = j;
    }
    if (!complete) out.push_back(goal);
    return true;
}

LiveRoute::~LiveRoute() {
    if (job_ && job_->thread.joinable()) job_->thread.join();
}

void LiveRoute::collect() {
    if (job_ && job_->done) {
        job_->thread.join();
        points_ = std::move(job_->out);
        from_ = job_->from;
        target_ = job_->to;
        job_.reset();
    }
}

void LiveRoute::clear() {
    collect();
    points_.clear();
}

void LiveRoute::request(const NavMesh& nav, const Vec3& from, const Vec3& to, uint64_t nowMs) {
    collect();
    if (job_) return;  // still working on the last one
    bool sameTarget = length(to - target_) < 1 && !points_.empty();
    // Re-route when the target changes, or every couple of seconds once you've moved off the start.
    if (sameTarget && (nowMs - lastMs_ < 2000 || length(from - from_) < 6)) return;
    lastMs_ = nowMs;
    job_ = std::make_unique<Job>();
    Job* job = job_.get();
    job->from = from;
    job->to = to;
    job->thread = std::thread([job, &nav] {
        nav.route(job->from, job->to, job->out);
        job->done = true;
    });
}

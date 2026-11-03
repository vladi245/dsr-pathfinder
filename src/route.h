#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "game.h"

// The game's navigation meshes as tools/build_guide.py joins them (navmesh.bin), so the overlay
// can route from wherever you are, such as a bonfire after dying, instead of only from where the
// guide's route starts. A port of tools/navmesh.py's routing.
class NavMesh {
public:
    bool load(const std::filesystem::path& path);
    bool load(std::string_view data);  // the same bytes, e.g. built into the exe
    bool loaded() const { return !tri_.empty(); }
    // Walking route from start to goal, both included. If the meshes don't connect all the way,
    // it goes as near as it can and then straight to the goal. False if either end is off the
    // meshes.
    bool route(const Vec3& start, const Vec3& goal, std::vector<Vec3>& out) const;

private:
    enum Kind : uint8_t { kWalk, kSeam, kGap, kDrop };
    struct Link {
        uint32_t to;
        float cost;
        uint8_t kind;
        Vec3 a, b;  // portal edge (a == b for gaps and drops)
    };

    std::vector<int> under(float x, float z) const;  // triangles whose outline contains (x, z)
    bool contains(int t, float x, float z) const;
    float height(int t, float x, float z) const;
    float distanceTo(int t, const Vec3& p) const;
    int surfaceAt(const Vec3& p, float maxRise) const;
    int nearest(const Vec3& p, float radius) const;
    bool walkable(const Vec3& p, const Vec3& q, const std::vector<uint8_t>& allowed) const;
    bool neighbours(int a, int b) const;

    std::vector<std::array<Vec3, 3>> tri_;
    std::vector<Vec3> centre_;
    std::vector<uint8_t> scrap_;
    std::vector<uint32_t> first_;
    std::vector<Link> links_;
    std::unordered_map<int64_t, std::vector<int>> grid_;  // 4 m cells
};

// Routes on a background thread and keeps the latest result.
class LiveRoute {
public:
    ~LiveRoute();
    // Asks for a route from `from` to `to`; a new one is only started when the last one is done
    // and you've moved or the target changed.
    void request(const NavMesh& nav, const Vec3& from, const Vec3& to, uint64_t nowMs);
    void clear();
    // The latest route, or empty. It starts where you were when it was asked for.
    const std::vector<Vec3>& points() const { return points_; }
    const Vec3& target() const { return target_; }

private:
    struct Job {
        std::atomic<bool> done{false};
        std::vector<Vec3> out;
        Vec3 from, to;
        std::thread thread;
    };
    void collect();

    std::unique_ptr<Job> job_;
    std::vector<Vec3> points_;
    Vec3 from_{}, target_{};
    uint64_t lastMs_ = 0;
};

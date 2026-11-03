#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "game.h"

struct Step {
    std::string title;
    std::string body;     // "text =" lines joined with newlines
    std::string warning;  // missable / point-of-no-return
    std::vector<Vec3> waypoints;   // route (yours if recorded, else the computed path) and goals
    std::vector<bool> isGoal;      // parallel to waypoints: a destination, not just a route point
    std::vector<uint32_t> flags;   // step completes itself when all of these event flags are on
    float radius = 0;              // 0 = use the config default
};

// The guide file: [step] blocks of "key = value" lines. Recording waypoints edits the file in
// place, keeping comments and layout, so the guide stays hand-editable.
class Guide {
public:
    bool load(const std::filesystem::path& path, std::string& error);
    const std::vector<Step>& steps() const { return steps_; }

    bool addWaypoint(size_t step, const Vec3& p);
    bool removeLastWaypoint(size_t step);

private:
    void parse();

    std::filesystem::path path_;
    std::vector<std::string> lines_;
    std::vector<Step> steps_;
    std::vector<std::vector<size_t>> wpLines_;  // per step: line index of each recorded waypoint
    std::vector<size_t> lastLine_;              // per step: last key line, for inserting
    std::vector<size_t> goalLine_;              // per step: first `path =`/`goal =` line, or npos
    std::string error_;
};

#include "guide.h"

#include <cstdio>
#include <cstdlib>

#include "textfile.h"

using textfile::keyValue;
using textfile::trim;

bool Guide::load(const std::filesystem::path& path, std::string& error) {
    path_ = path;
    if (!textfile::readLines(path, lines_)) {
        error = "can't open " + path.u8string();
        return false;
    }
    parse();
    error = error_;
    return error.empty();
}

void Guide::parse() {
    steps_.clear();
    wpLines_.clear();
    lastLine_.clear();
    goalLine_.clear();
    error_.clear();
    struct RoutePoint {
        Vec3 p;
        bool goal;
    };
    std::vector<std::vector<RoutePoint>> routes;  // per step: computed path and goals, in order

    for (size_t i = 0; i < lines_.size(); ++i) {
        std::string t = trim(lines_[i]);
        if (t == "[step]") {
            steps_.emplace_back();
            wpLines_.emplace_back();
            lastLine_.push_back(i);
            goalLine_.push_back(std::string::npos);
            routes.emplace_back();
            continue;
        }
        std::string key, value;
        if (!keyValue(lines_[i], key, value)) continue;
        if (steps_.empty()) {
            error_ = "line " + std::to_string(i + 1) + ": '" + key + "' before the first [step]";
            continue;
        }
        Step& s = steps_.back();
        lastLine_.back() = i;

        if (key == "title") {
            s.title = value;
        } else if (key == "text") {
            if (!s.body.empty()) s.body += "\n";
            s.body += value;
        } else if (key == "warning") {
            if (!s.warning.empty()) s.warning += "\n";
            s.warning += value;
        } else if (key == "flag") {
            // "flag = 51010000" or "flag = 51010000, 51010010": all must be on
            for (const char* p = value.c_str(); *p;) {
                char* end;
                unsigned long f = std::strtoul(p, &end, 10);
                if (end == p) {
                    ++p;
                    continue;
                }
                s.flags.push_back(static_cast<uint32_t>(f));
                p = end;
            }
        } else if (key == "radius") {
            s.radius = std::strtof(value.c_str(), nullptr);
        } else if (key == "wp" || key == "goal" || key == "path") {
            Vec3 p;
            if (std::sscanf(value.c_str(), "%f , %f , %f", &p.x, &p.y, &p.z) != 3) {
                error_ = "line " + std::to_string(i + 1) + ": " + key + " needs 'x, y, z'";
            } else if (key == "wp") {
                s.waypoints.push_back(p);
                s.isGoal.push_back(false);
                wpLines_.back().push_back(i);
            } else {
                routes.back().push_back({p, key == "goal"});
                if (goalLine_.back() == std::string::npos) goalLine_.back() = i;
            }
        }
    }
    // Your recorded route comes first and replaces the computed path; goals always follow.
    for (size_t s = 0; s < steps_.size(); ++s) {
        bool recorded = !wpLines_[s].empty();
        for (const RoutePoint& r : routes[s]) {
            if (recorded && !r.goal) continue;
            steps_[s].waypoints.push_back(r.p);
            steps_[s].isGoal.push_back(r.goal);
        }
    }
}

bool Guide::addWaypoint(size_t step, const Vec3& p) {
    if (step >= steps_.size()) return false;
    char buf[96];
    std::snprintf(buf, sizeof buf, "wp = %.2f, %.2f, %.2f", p.x, p.y, p.z);
    // After the last recorded waypoint; otherwise just before the goals, so the route leads to them.
    size_t at = !wpLines_[step].empty()                 ? wpLines_[step].back() + 1
                : goalLine_[step] != std::string::npos ? goalLine_[step]
                                                        : lastLine_[step] + 1;
    lines_.insert(lines_.begin() + at, buf);
    if (!textfile::writeLines(path_, lines_)) return false;
    parse();
    return true;
}

bool Guide::removeLastWaypoint(size_t step) {
    if (step >= steps_.size() || wpLines_[step].empty()) return false;
    lines_.erase(lines_.begin() + wpLines_[step].back());
    if (!textfile::writeLines(path_, lines_)) return false;
    parse();
    return true;
}

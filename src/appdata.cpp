#include "appdata.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "resource.h"
#include "textfile.h"

namespace fs = std::filesystem;

namespace appdata {

namespace {
fs::path exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

bool writeBytes(const fs::path& path, std::string_view data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

// FNV-1a, to tell whether the built-in guide changed since your copy was made.
std::string fingerprint(std::string_view data) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : data) h = (h ^ c) * 1099511628211ull;
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::vector<std::string> splitLines(std::string_view data) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= data.size()) {
        size_t end = data.find('\n', start);
        if (end == std::string_view::npos) end = data.size();
        std::string line(data.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        start = end + 1;
    }
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

// The new guide's lines with your recorded "wp =" lines from the old one put back under each
// step's id (steps are matched by id, which stays stable across guide versions).
std::vector<std::string> keepWaypoints(const std::vector<std::string>& fresh, const std::vector<std::string>& old) {
    std::map<std::string, std::vector<std::string>> recorded;
    std::string id;
    for (const std::string& line : old) {
        std::string key, value;
        if (textfile::trim(line) == "[step]") id.clear();
        else if (textfile::keyValue(line, key, value) && key == "id") id = value;
        else if (textfile::keyValue(line, key, value) && key == "wp" && !id.empty()) recorded[id].push_back(line);
    }
    std::vector<std::string> out;
    for (const std::string& line : fresh) {
        out.push_back(line);
        std::string key, value;
        if (textfile::keyValue(line, key, value) && key == "id") {
            auto it = recorded.find(value);
            if (it != recorded.end()) out.insert(out.end(), it->second.begin(), it->second.end());
        }
    }
    return out;
}
}  // namespace

std::string_view embedded(int id) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!res) return {};
    HGLOBAL data = LoadResource(nullptr, res);
    if (!data) return {};
    return {static_cast<const char*>(LockResource(data)), SizeofResource(nullptr, res)};
}

bool portable() {
    std::error_code ec;
    return fs::exists(exeDir() / "config.ini", ec);
}

fs::path folder() {
    if (portable()) return exeDir();
    fs::path dir;
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        dir = fs::path(roaming) / L"DSR Pathfinder";
        CoTaskMemFree(roaming);
    } else {
        dir = exeDir();
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void prepare(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir / "config.ini", ec)) {
        std::string_view config = embedded(IDR_CONFIG);
        if (!config.empty()) writeBytes(dir / "config.ini", config);
    }

    std::string_view guide = embedded(IDR_GUIDE);
    if (guide.empty()) return;  // a build without the guide inside: use whatever is in the folder
    const fs::path mine = dir / "guide.txt", stamp = dir / "guide.version";
    // A portable copy (or the development folder) keeps its own guide.txt, which you may have
    // regenerated since this exe was built.
    if (portable() && fs::exists(mine, ec)) return;
    std::vector<std::string> seen;
    textfile::readLines(stamp, seen);
    const std::string print = fingerprint(guide);
    if (fs::exists(mine, ec) && !seen.empty() && seen[0] == print) return;  // up to date

    std::vector<std::string> fresh = splitLines(guide), old;
    if (fs::exists(mine, ec) && textfile::readLines(mine, old)) {
        // A newer guide: keep a copy of yours, then carry your recorded waypoints over.
        fs::copy_file(mine, dir / "guide.previous.txt", fs::copy_options::overwrite_existing, ec);
        fresh = keepWaypoints(fresh, old);
        std::printf("Updated guide.txt to the version built into this app, keeping your waypoints\n");
    }
    textfile::writeLines(mine, fresh);
    textfile::writeLines(stamp, {print});
}

}  // namespace appdata

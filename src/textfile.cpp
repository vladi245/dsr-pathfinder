#include "textfile.h"

#include <windows.h>

#include <cstdlib>
#include <fstream>

namespace textfile {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

bool keyValue(const std::string& line, std::string& key, std::string& value) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = trim(t.substr(0, eq));
    value = trim(t.substr(eq + 1));
    return !key.empty();
}

bool readLines(const std::filesystem::path& path, std::vector<std::string>& lines) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    lines.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    // Drop a UTF-8 byte order mark if an editor added one.
    if (!lines.empty() && lines[0].rfind("\xEF\xBB\xBF", 0) == 0) lines[0].erase(0, 3);
    return true;
}

bool writeLines(const std::filesystem::path& path, const std::vector<std::string>& lines) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (auto& l : lines) out << l << "\r\n";
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
}

bool KeyValueFile::load(const std::filesystem::path& path) {
    path_ = path;
    return readLines(path, lines_);
}

bool KeyValueFile::save() const { return writeLines(path_, lines_); }

std::string KeyValueFile::get(const std::string& key, const std::string& fallback) const {
    std::string k, v;
    for (auto& l : lines_) {
        if (keyValue(l, k, v) && k == key) {
            size_t c = v.find_first_of("#;");  // allow trailing comments in config values
            return trim(v.substr(0, c));
        }
    }
    return fallback;
}

float KeyValueFile::getFloat(const std::string& key, float fallback) const {
    std::string v = get(key);
    return v.empty() ? fallback : std::strtof(v.c_str(), nullptr);
}

int KeyValueFile::getInt(const std::string& key, int fallback) const {
    std::string v = get(key);
    return v.empty() ? fallback : std::atoi(v.c_str());
}

void KeyValueFile::set(const std::string& key, const std::string& value) {
    std::string k, v;
    for (auto& l : lines_) {
        if (keyValue(l, k, v) && k == key) {
            l = key + " = " + value;
            return;
        }
    }
    lines_.push_back(key + " = " + value);
}

}  // namespace textfile

#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Small helpers for the line-based "key = value" files (config, guide, progress).
namespace textfile {

std::string trim(const std::string& s);
// Splits "key = value"; false for blank lines, comments (# or ;) and section headers.
bool keyValue(const std::string& line, std::string& key, std::string& value);
bool readLines(const std::filesystem::path& path, std::vector<std::string>& lines);
bool writeLines(const std::filesystem::path& path, const std::vector<std::string>& lines);
std::wstring widen(const std::string& utf8);

// A "key = value" file that keeps comments and ordering when values are changed.
class KeyValueFile {
public:
    bool load(const std::filesystem::path& path);
    bool save() const;
    std::string get(const std::string& key, const std::string& fallback = "") const;
    float getFloat(const std::string& key, float fallback) const;
    int getInt(const std::string& key, int fallback) const;
    void set(const std::string& key, const std::string& value);

private:
    std::filesystem::path path_;
    std::vector<std::string> lines_;
};

}  // namespace textfile

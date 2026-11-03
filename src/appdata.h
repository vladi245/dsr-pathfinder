#pragma once
#include <filesystem>
#include <string_view>

// Where the app keeps your files, and the files built into it.
namespace appdata {

// A file built into the exe (see resource.h), or empty.
std::string_view embedded(int id);

// The folder for settings, progress, your copy of the guide and the log. Next to the exe when a
// config.ini is there (a portable copy, or the development folder), else
// %APPDATA%\DSR Pathfinder. Created if needed.
std::filesystem::path folder();

// True when folder() is next to the exe rather than in %APPDATA%.
bool portable();

// Puts the built-in config.ini and guide.txt into `dir` if they're missing. When the built-in
// guide is newer than your copy, it replaces it but keeps the waypoints you recorded with F7.
void prepare(const std::filesystem::path& dir);

}  // namespace appdata

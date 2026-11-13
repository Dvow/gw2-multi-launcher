#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace gw2::setup_paths {
namespace fs = std::filesystem;

inline bool file(const fs::path &path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}
// GW2's archive is one Gw2.dat file, or a Gw2.dat folder of chunk files the game locks
// with Gw2.dat\Lock.dat. The China (KongZhong) installer creates the folder form.
inline bool archive(const fs::path &path) {
    std::error_code error;
    return file(path) || fs::is_directory(path, error);
}
inline fs::path normalized(const fs::path &path) {
    std::error_code error;
    auto result = fs::weakly_canonical(path, error);
    return error ? path.lexically_normal() : result;
}
inline void unique(std::vector<fs::path> &paths, const fs::path &path) {
    const auto value = normalized(path);
    if (std::find(paths.begin(), paths.end(), value) == paths.end()) paths.push_back(value);
}
inline std::string lower(std::string text) {
    for (auto &c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}
// Why this path cannot start GW2, or empty when it can. Detection offers only paths that pass.
inline std::string gameProblem(const fs::path &path) {
    const auto name = path.filename().u8string();
    if (path.empty()) return "Choose your Gw2-64.exe.";
    if (!path.is_absolute() || lower(std::string(name.begin(), name.end())) != "gw2-64.exe")
        return "Choose the Gw2-64.exe file in your Guild Wars 2 folder.";
    if (!file(path)) return "Gw2-64.exe is no longer at its saved location. Find it again or browse to it.";
    if (!archive(path.parent_path() / "Gw2.dat"))
        return "Gw2.dat is missing beside Gw2-64.exe. Finish installing Guild Wars 2 or choose another copy.";
    return {};
}
inline bool gameExecutable(const fs::path &path) { return gameProblem(path).empty(); }
inline std::vector<fs::path> children(const fs::path &directory) {
    std::vector<fs::path> result;
    std::error_code error;
    fs::directory_iterator item(directory, fs::directory_options::skip_permission_denied, error), end;
    for (unsigned count = 0; !error && item != end && count < 512; item.increment(error), ++count)
        result.push_back(item->path());
    std::sort(result.begin(), result.end());
    return result;
}
inline std::string readSmall(const fs::path &path) {
    std::error_code error;
    if (!file(path) || fs::file_size(path, error) > 1024 * 1024 || error) return {};
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
// VDF values are data: only parse quoted strings, never expand shell commands.
inline std::vector<std::string> quotedValues(std::string_view text) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        std::string value;
        bool complete{};
        while (++i < text.size()) {
            if (text[i] == '"') { complete = true; break; }
            if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '\\' || text[i + 1] == '"')) ++i;
            value += text[i];
        }
        if (complete) result.push_back(std::move(value));
    }
    return result;
}
} // namespace gw2::setup_paths

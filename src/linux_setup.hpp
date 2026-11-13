#pragma once

#include "setup_paths.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace gw2::linux_setup {
using namespace setup_paths;

struct Environment {
    fs::path home, data;
    std::string searchPath;
    fs::path winePrefix, protonPath;
};
struct Installation { fs::path game, prefix; };
struct Selection { std::string game, runner, prefix, proton; };
struct Detection {
    std::vector<Installation> games;
    std::vector<fs::path> prefixes, protons;
    std::vector<std::string> runners;
    fs::path preferredPrefix, preferredProton;
};

inline std::string environment(const char *name) {
    const auto value = std::getenv(name);
    return value ? value : "";
}
inline Environment currentEnvironment() {
    Environment result{environment("HOME"), environment("XDG_DATA_HOME"), environment("PATH"),
        environment("WINEPREFIX"), environment("PROTONPATH")};
    if (!result.data.is_absolute()) result.data = result.home / ".local/share";
    return result;
}
inline bool executable(const fs::path &path) {
    return file(path) && ::access(path.c_str(), X_OK) == 0;
}
inline fs::path findExecutable(std::string_view command, std::string_view searchPath) {
    if (command.empty()) return {};
    fs::path supplied{command};
    if (supplied.is_absolute()) return executable(supplied) ? supplied : fs::path{};
    if (supplied.has_parent_path()) return {};
    while (!searchPath.empty()) {
        const auto end = searchPath.find(':');
        const fs::path directory{searchPath.substr(0, end)};
        if (directory.is_absolute() && executable(directory / supplied)) return directory / supplied;
        if (end == searchPath.npos) break;
        searchPath.remove_prefix(end + 1);
    }
    return {};
}
inline bool prefix(const fs::path &path) { return path.is_absolute() && file(path / "system.reg"); }
inline bool proton(const fs::path &path) { return path.is_absolute() && executable(path / "proton"); }
inline fs::path configPath(std::string value, const fs::path &home) {
    const auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
    if (first == value.npos) return {};
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\'' && value.back() == '\''))) value = value.substr(1, value.size() - 2);
    for (const std::string_view start : {"~/", "$HOME/", "${HOME}/"})
        if (value.starts_with(start)) return home / value.substr(start.size());
    fs::path result(value);
    return result.is_absolute() ? result : fs::path{};
}
inline Detection detect(const Environment &env) {
    Detection out;
    if (!env.home.is_absolute()) return out;
    const auto addPrefix = [&](const fs::path &path) { if (prefix(path)) unique(out.prefixes, path); };
    const auto addProton = [&](const fs::path &path) { if (proton(path)) unique(out.protons, path); };
    const auto addGame = [&](const fs::path &game, const fs::path &winePrefix = {}) {
        if (!gameExecutable(game)) return;
        const auto real = normalized(game);
        auto found = std::find_if(out.games.begin(), out.games.end(), [&](const auto &item) { return item.game == real; });
        const auto associated = prefix(winePrefix) ? normalized(winePrefix) : fs::path{};
        if (found == out.games.end()) out.games.push_back({real, associated});
        else if (found->prefix.empty()) found->prefix = associated;
        addPrefix(winePrefix);
    };
    for (const auto *runner : {"umu-run", "wine", "wine64"})
        if (!findExecutable(runner, env.searchPath).empty()) out.runners.emplace_back(runner);
    if (prefix(env.winePrefix)) { out.preferredPrefix = normalized(env.winePrefix); addPrefix(env.winePrefix); }
    if (proton(env.protonPath)) { out.preferredProton = normalized(env.protonPath); addProton(env.protonPath); }

    std::vector<fs::path> steamRoots, libraries;
    for (const auto &root : {env.data / "Steam", env.home / ".steam/steam", env.home / ".steam/root",
             env.home / ".var/app/com.valvesoftware.Steam/data/Steam"}) unique(steamRoots, root);
    for (const auto &root : steamRoots) {
        unique(libraries, root);
        const auto words = quotedValues(readSmall(root / "steamapps/libraryfolders.vdf"));
        for (std::size_t i = 0; i + 1 < words.size(); ++i)
            if (words[i] == "path" && fs::path(words[i + 1]).is_absolute()) unique(libraries, words[i + 1]);
        for (const auto &tool : children(root / "compatibilitytools.d")) addProton(tool);
    }
    for (const auto &library : libraries) {
        const auto pfx = library / "steamapps/compatdata/1284210/pfx";
        addPrefix(pfx);
        addGame(library / "steamapps/common/Guild Wars 2/Gw2-64.exe", pfx);
        for (const auto &tool : children(library / "steamapps/common"))
            if (lower(tool.filename().string()).starts_with("proton")) addProton(tool);
    }
    for (const auto &root : {env.data / "umu/compatibilitytools", env.data / "lutris/runners/proton"})
        for (const auto &tool : children(root)) addProton(tool);
    for (const auto &tool : children(env.data / "lutris/runners/wine"))
        if (executable(tool / "bin/wine")) out.runners.push_back((tool / "bin/wine").string());

    addPrefix(env.home / ".wine");
    for (const auto &game : children(env.home / "Games")) {
        addPrefix(game);
        addPrefix(game / "pfx");
        auto associated = prefix(game) ? game : prefix(game / "pfx") ? game / "pfx" : fs::path{};
        if (associated.empty() && prefix(fs::path(game.string() + "-prefix"))) associated = fs::path(game.string() + "-prefix");
        addGame(game / "Gw2-64.exe", associated);
    }
    for (const auto &root : {env.data / "bottles/bottles",
             env.home / ".var/app/com.usebottles.bottles/data/bottles/bottles"})
        for (const auto &bottle : children(root)) addPrefix(bottle);
    for (const auto &config : children(env.home / ".config/lutris/games")) {
        if (config.extension() != ".yml" && config.extension() != ".yaml") continue;
        std::istringstream input(readSmall(config));
        fs::path game, winePrefix;
        std::string line;
        std::size_t bytes{};
        while (std::getline(input, line) && (bytes += line.size()) < 1024 * 1024) {
            const auto begin = line.find_first_not_of(" \t");
            if (begin == line.npos) continue;
            const auto field = std::string_view(line).substr(begin);
            if (field.starts_with("exe:")) game = configPath(std::string(field.substr(4)), env.home);
            if (field.starts_with("prefix:")) winePrefix = configPath(std::string(field.substr(7)), env.home);
        }
        addGame(game, winePrefix);
    }
    // Inspect conventional game folders inside known prefixes, not the whole disk.
    for (const auto &pfx : std::vector<fs::path>(out.prefixes))
        for (const auto *base : {"drive_c/Program Files", "drive_c/Program Files (x86)", "drive_c/Games", "drive_c"})
            for (const auto &directory : children(pfx / base))
                if (lower(directory.filename().string()).find("guild") != std::string::npos ||
                    lower(directory.filename().string()).find("gw2") != std::string::npos)
                    addGame(directory / "Gw2-64.exe", pfx);
    return out;
}
inline Selection defaults(const Detection &found, const fs::path &current = {}) {
    Selection out;
    const bool keep = gameExecutable(current);
    const auto saved = keep ? normalized(current) : fs::path{};
    const Installation *best{};
    const auto rank = [&](const Installation &game) {
        return prefix(game.prefix) ? (game.prefix == found.preferredPrefix ? 2 : 1) : 0;
    };
    for (const auto &game : found.games) {
        if (!gameExecutable(game.game)) continue;
        if (keep) {
            if (game.game == saved) { best = &game; break; }
        } else if (!best || rank(game) > rank(*best)) best = &game;
    }
    if (keep) out.game = current.string();
    else if (best) out.game = best->game.string();
    if (best) out.prefix = best->prefix.string();
    if (out.prefix.empty() && !found.preferredPrefix.empty()) out.prefix = found.preferredPrefix.string();
    if (!found.runners.empty()) out.runner = found.runners.front();
    if (!found.preferredProton.empty()) out.proton = found.preferredProton.string();
    else if (found.protons.size() == 1) out.proton = found.protons.front().string();
    return out;
}
inline void fillMissing(Selection &current, const Selection &suggested) {
    if (!gameExecutable(current.game) && gameExecutable(suggested.game)) {
        current.game = suggested.game;
        current.prefix = suggested.prefix;
    }
    if (current.prefix.empty() && !suggested.game.empty() && normalized(current.game) == normalized(suggested.game))
        current.prefix = suggested.prefix;
    if (current.runner.empty()) current.runner = suggested.runner;
    if (current.proton.empty()) current.proton = suggested.proton;
}
inline std::vector<std::string> problems(const Selection &value, std::string_view searchPath) {
    std::vector<std::string> result;
    if (auto game = gameProblem(value.game); !game.empty()) result.push_back(std::move(game));
    if (findExecutable(value.runner, searchPath).empty())
        result.emplace_back("Choose an installed Wine or UMU runner.");
    if (!prefix(value.prefix)) result.emplace_back("Choose a Wine prefix containing system.reg.");
    if (fs::path(value.runner).filename() == "umu-run" && !proton(value.proton))
        result.emplace_back("Choose a Proton folder containing proton.");
    return result;
}
} // namespace gw2::linux_setup

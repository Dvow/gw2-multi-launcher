#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <nlohmann/json.hpp>
#include "setup_paths.hpp"

namespace gw2::windows_setup {
using namespace setup_paths;
struct Environment {
    std::vector<fs::path> registeredGames, steamRoots, gameFolders, epicData;
    fs::path epicInstalled;
    // WeGame's game libraries (WeGameApps\rail_apps): the Chinese client's WeGame edition is
    // one of their folders, named after the game.
    std::vector<fs::path> wegameLibraries;
};
struct Detection { std::vector<fs::path> games; };

inline std::wstring registryString(HKEY root, const wchar_t *key, const wchar_t *name, DWORD view) {
    DWORD bytes{};
    const auto flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view;
    if (RegGetValueW(root, key, name, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes > 65536) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    value.resize(value.find(L'\0'));
    return value;
}
inline fs::path environmentPath(const wchar_t *name) {
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size || size > 32768) return {};
    std::wstring value(size, L'\0');
    const auto read = GetEnvironmentVariableW(name, value.data(), size);
    if (!read || read >= size) return {};
    value.resize(read);
    fs::path path(value);
    return path.is_absolute() ? path : fs::path{};
}
inline Environment currentEnvironment() {
    Environment out;
    for (const auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
        for (const DWORD view : {RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY}) {
            for (const auto *name : {L"Path", L"InstallPath"}) {
                const fs::path value(registryString(root, L"SOFTWARE\\ArenaNet\\Guild Wars 2", name, view));
                if (value.is_absolute()) unique(out.registeredGames, value);
            }
            // The China (KongZhong) installer's own uninstall entry (Inno Setup AppId).
            const fs::path china(registryString(root,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{E7E66EE2-9223-41CD-A296-F7F29C789927}_is1",
                L"InstallLocation", view));
            if (china.is_absolute()) unique(out.registeredGames, china);
            for (const auto *name : {L"SteamPath", L"InstallPath"}) {
                const fs::path value(registryString(root, L"SOFTWARE\\Valve\\Steam", name, view));
                if (value.is_absolute()) unique(out.steamRoots, value);
            }
            const fs::path epic(registryString(root, L"SOFTWARE\\Epic Games\\EpicGamesLauncher", L"AppDataPath", view));
            if (epic.is_absolute()) unique(out.epicData, epic);
        }
    const auto shared = environmentPath(L"ProgramData");
    if (!shared.empty()) {
        unique(out.epicData, shared / L"Epic/EpicGamesLauncher/Data");
        out.epicInstalled = shared / L"Epic/UnrealEngineLauncher/LauncherInstalled.dat";
    }
    for (const auto *name : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
        const auto folder = environmentPath(name);
        if (!folder.empty()) { unique(out.gameFolders, folder); unique(out.steamRoots, folder / L"Steam"); }
    }
    const auto home = environmentPath(L"USERPROFILE");
    if (!home.empty()) unique(out.gameFolders, home / L"Games");
    const auto local = environmentPath(L"LOCALAPPDATA");
    if (!local.empty()) unique(out.gameFolders, local / L"Programs");
    // Only fixed local drives and conventional folders; never crawl whole disks.
    const auto drives = GetLogicalDrives();
    for (unsigned i = 0; i < 26; ++i) {
        if (!(drives & (1UL << i))) continue;
        const wchar_t drive[]{static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        if (GetDriveTypeW(drive) != DRIVE_FIXED) continue;
        for (const auto *folder : {L"", L"Games", L"Program Files", L"Program Files (x86)"})
            unique(out.gameFolders, fs::path(drive) / folder);
        unique(out.steamRoots, fs::path(drive) / L"SteamLibrary");
        unique(out.steamRoots, fs::path(drive) / L"Steam");
        unique(out.wegameLibraries, fs::path(drive) / L"WeGameApps" / L"rail_apps");
    }
    return out;
}
inline bool samePath(const fs::path &a, const fs::path &b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
inline Detection detect(const Environment &env) {
    Detection out;
    const auto add = [&](fs::path path) {
        if (!path.is_absolute()) return;
        if (!samePath(path.filename(), L"Gw2-64.exe")) path /= L"Gw2-64.exe";
        if (!gameExecutable(path)) return;
        path = normalized(path);
        if (std::none_of(out.games.begin(), out.games.end(), [&](const auto &other) { return samePath(path, other); }))
            out.games.push_back(std::move(path));
    };
    for (const auto &game : env.registeredGames) add(game);
    for (const auto &folder : env.gameFolders)
        for (const auto *name : {L"Guild Wars 2", L"GuildWars2", L"GW2"}) add(folder / name);
    std::vector<fs::path> libraries;
    for (const auto &root : env.steamRoots) {
        if (!root.is_absolute()) continue;
        unique(libraries, root);
        const auto values = quotedValues(readSmall(root / "steamapps/libraryfolders.vdf"));
        for (std::size_t i = 0; i + 1 < values.size(); ++i) {
            if (values[i] != "path") continue;
            const auto &text = values[i + 1];
            const fs::path library(std::u8string_view(reinterpret_cast<const char8_t *>(text.data()), text.size()));
            if (library.is_absolute()) unique(libraries, library);
        }
    }
    for (const auto &library : libraries) add(library / L"steamapps/common/Guild Wars 2");
    for (const auto &library : env.wegameLibraries)
        if (library.is_absolute())
            for (const auto &folder : children(library)) add(folder);
    const auto addEpic = [&](const nlohmann::json &entry) {
        if (!entry.is_object()) return;
        const auto location = entry.find("InstallLocation");
        if (location == entry.end() || !location->is_string()) return;
        const auto &text = location->get_ref<const std::string &>();
        if (text.find('\0') != text.npos) return;
        add(fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(text.data()), text.size())));
    };
    for (const auto &data : env.epicData) {
        if (!data.is_absolute()) continue;
        for (const auto &manifest : children(data / L"Manifests"))
            if (samePath(manifest.extension(), L".item"))
                addEpic(nlohmann::json::parse(readSmall(manifest), nullptr, false));
    }
    if (env.epicInstalled.is_absolute()) {
        const auto metadata = nlohmann::json::parse(readSmall(env.epicInstalled), nullptr, false);
        const auto list = metadata.find("InstallationList");
        if (list != metadata.end() && list->is_array())
            for (const auto &entry : *list) addEpic(entry);
    }
    return out;
}
inline fs::path defaultGame(const Detection &found, const fs::path &current) {
    if (gameExecutable(current)) return current;
    // Discovery prefers registered installs, then conventional folders, then provider libraries.
    const auto best = std::find_if(found.games.begin(), found.games.end(), gameExecutable);
    return best == found.games.end() ? current : *best;
}
} // namespace gw2::windows_setup

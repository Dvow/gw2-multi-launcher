module;
#include <SDL3/SDL.h>
#include <array>
#include <charconv>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include "package_update.hpp"
#endif

export module update:common;
import platform;
import data_folder;

export namespace gw2 {
inline constexpr std::string_view appVersion = GW2_APP_VERSION;
enum class UpdateStage { idle, checking, current, available, installing, installed };
struct AppUpdate {
    UpdateStage stage{UpdateStage::idle};
    std::string version, url, digest, error;
    std::size_t size{};
    bool busy() const { return stage == UpdateStage::checking || stage == UpdateStage::installing; }
};

std::array<unsigned, 3> versionParts(std::string_view text);
bool appUpdatesEnabled();
bool appUpdatesAlwaysOn();
std::string_view appUpdateNotice();
AppUpdate checkAppUpdate(std::stop_token stop);
std::filesystem::path downloadAppUpdate(const AppUpdate &release, std::stop_token stop);
void installAppUpdate(const AppUpdate &release, std::stop_token stop);
void relaunchUpdatedApp();
void relaunchApp();
void clearParkedFiles();
} // namespace gw2

namespace gw2 {
std::array<unsigned, 3> versionParts(std::string_view text) {
    std::array<unsigned, 3> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto end = text.find('.');
        const auto part = text.substr(0, end);
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), result[i]);
        if (part.empty() || (part.size() > 1 && part.front() == '0') || parsed.ec != std::errc{} ||
            parsed.ptr != part.data() + part.size() || (end == text.npos) != (i == 2))
            throw std::runtime_error("The release has an invalid version.");
        text.remove_prefix(part.size() + (end != text.npos));
    }
    return result;
}
void installAppUpdate(const AppUpdate &release, std::stop_token stop) {
    const auto file = downloadAppUpdate(release, stop);
#ifdef _WIN32
    std::vector<std::string> args{
        utf8(file), "/SILENT", "/NORESTART", "/UPDATEPID=" + std::to_string(GetCurrentProcessId())};
    const auto base = SDL_GetBasePath();
    if (base && (std::filesystem::exists(path(base) / "unins000.exe") ||
            std::filesystem::exists(path(base) / ("Uninstall " + std::string(gw2ProductName()) + ".exe"))))
        args.push_back("/DIR=" + std::string(base));
    std::vector<const char *> pointers;
    for (const auto &arg : args)
        pointers.push_back(arg.c_str());
    pointers.push_back(nullptr);
    cancelled(stop);
    auto process = std::unique_ptr<SDL_Process, decltype(&SDL_DestroyProcess)>(
        SDL_CreateProcess(pointers.data(), false), SDL_DestroyProcess);
    if (!process) throw std::runtime_error("Could not start the update installer.");
#else
    const auto base = SDL_GetBasePath();
    if (!base) throw std::runtime_error("Cannot locate the installed launcher.");
    const auto args = package::installCommand(package::format(path(base)), file);
    const auto directory = dataRoot() / "launcher-update";
    std::filesystem::create_directories(directory);
    cancelled(stop);
    package::install(args, directory / "install.log");
#endif
}
void removeParked(const std::filesystem::path &file) {
    std::error_code error;
    const auto prefix = file.filename().string() + ".old";
    for (const auto &entry : std::filesystem::directory_iterator(file.parent_path(), error)) {
        if (error) return;
        if (!entry.is_regular_file(error)) continue;
        if (entry.path().filename().string().starts_with(prefix)) std::filesystem::remove(entry.path(), error);
    }
}
void clearParkedFiles() {
#ifdef _WIN32
    const auto base = SDL_GetBasePath();
    if (!base) return;
    const std::filesystem::path root(base);
    for (const char *name : {GW2_PROGRAM_FILE ".exe", "GW2MultiLauncher.exe",
             GW2_PROGRAM_FILE ".Host.exe", "GW2MultiLauncher.Host.exe",
             GW2_PROGRAM_FILE ".Native.dll", "GW2MultiLauncher.Native.dll",
             GW2_PROGRAM_FILE ".Steam.exe", "GW2MultiLauncher.Steam.exe",
             GW2_PROGRAM_FILE ".Steam.dll", "GW2MultiLauncher.Steam.dll",
             "steam/" GW2_PROGRAM_FILE ".Steam.exe", "steam/GW2MultiLauncher.Steam.exe",
             "steam/" GW2_PROGRAM_FILE ".Steam.dll", "steam/GW2MultiLauncher.Steam.dll"})
        removeParked(root / name);
#endif
}
void relaunchUpdatedApp() {
#ifndef _WIN32
    try {
        relaunchApp();
    } catch (const std::exception &) {
        throw std::runtime_error(std::string("The update was installed. Reopen ") + gw2ProductName() + ".");
    }
#endif
}
// Starts a new launcher after this one released its instance lock.
void relaunchApp() {
    const auto base = SDL_GetBasePath();
    if (!base) throw std::runtime_error(std::string("Reopen ") + gw2ProductName() + ".");
#ifdef _WIN32
    const auto executable = std::string(base) + GW2_PROGRAM_FILE ".exe";
#else
    const auto executable = std::string(base) + GW2_PROGRAM_FILE;
#endif
    const char *args[]{executable.c_str(), nullptr};
    auto process = SDL_CreateProcess(args, false);
    if (!process) throw std::runtime_error(std::string("Reopen ") + gw2ProductName() + ".");
    SDL_DestroyProcess(process);
}
} // namespace gw2

module;
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

export module update;
import platform;

export namespace gw2 {
inline constexpr std::string_view appVersion = GW2_APP_VERSION;
enum class UpdateStage { idle, checking, current, available, installing, installed };
struct AppUpdate {
    UpdateStage stage{UpdateStage::idle};
    std::string version, url, digest, error;
    std::size_t size{};
    bool busy() const { return stage == UpdateStage::checking || stage == UpdateStage::installing; }
};

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
AppUpdate parseAppRelease(std::string_view body, std::string_view current = appVersion) {
    const auto json = nlohmann::json::parse(body);
    AppUpdate result;
    result.stage = UpdateStage::current;
    if (json.at("draft").get<bool>() || json.at("prerelease").get<bool>()) return result;
    const auto tag = json.at("tag_name").get<std::string>();
    if (!tag.starts_with('v')) throw std::runtime_error("The release has an invalid version tag.");
    const auto version = tag.substr(1);
    if (versionParts(version) <= versionParts(current)) return result;
#ifdef _WIN32
    const auto name = "gw2-multi-launcher_" + version + "_win-x64_setup.exe";
#else
    const auto name = "gw2-multi-launcher_" + version + "_amd64.deb";
#endif
    const auto expectedUrl =
        "https://github.com/Dvow/gw2-multi-launcher/releases/download/" + tag + "/" + name;
    for (const auto &asset : json.at("assets")) {
        if (asset.at("name") != name) continue;
        if (!result.version.empty()) throw std::runtime_error("The release contains duplicate installers.");
        const auto digest = asset.at("digest").get<std::string>();
        const auto size = asset.at("size").get<std::int64_t>();
        if (asset.at("state") != "uploaded" || asset.at("browser_download_url") != expectedUrl ||
            !digest.starts_with("sha256:") || digest.size() != 71 ||
            digest.find_first_not_of("0123456789abcdef", 7) != digest.npos || size <= 0 ||
            size > 128 * 1024 * 1024)
            throw std::runtime_error("The release installer could not be verified.");
        result = {UpdateStage::available, version, expectedUrl, digest.substr(7), {},
            static_cast<std::size_t>(size)};
    }
    if (result.version.empty()) throw std::runtime_error("This release has no installer for your platform.");
    return result;
}
AppUpdate checkAppUpdate(std::stop_token stop) {
    auto reply =
        https("api.github.com", "/repos/Dvow/gw2-multi-launcher/releases/latest", {}, {}, stop, 131072);
    if (reply.status == 404) return {UpdateStage::current, {}, {}, {}, {}, 0};
    if (reply.status != 200) throw std::runtime_error("Could not check GitHub for updates. Try again later.");
    try {
        return parseAppRelease(reply.body.text());
    } catch (const nlohmann::json::exception &) {
        throw std::runtime_error("GitHub returned invalid release information. Try again later.");
    }
}
std::filesystem::path downloadAppUpdate(const AppUpdate &release, std::stop_token stop) {
    constexpr std::string_view prefix = "https://github.com";
    if (release.stage != UpdateStage::available || !release.url.starts_with(prefix) ||
        versionParts(release.version) <= versionParts(appVersion))
        throw std::runtime_error("Check for updates before installing.");
    auto reply =
        https("github.com", std::string_view(release.url).substr(prefix.size()), {}, {}, stop, release.size);
    if (reply.status != 200 || reply.body.bytes.size() != release.size ||
        sha256(reply.body.bytes) != release.digest)
        throw std::runtime_error("The download failed verification. Check for updates and try again.");
#ifdef _WIN32
    const auto file = dataRoot() / "update.exe";
#else
    const auto file = dataRoot() / "update.deb";
#endif
    cancelled(stop);
    atomicWrite(file, reply.body.text());
    return file;
}
void installAppUpdate(const AppUpdate &release, std::stop_token stop) {
    const auto file = downloadAppUpdate(release, stop);
#ifdef _WIN32
    std::vector<std::string> args{
        utf8(file), "/SILENT", "/NORESTART", "/UPDATEPID=" + std::to_string(GetCurrentProcessId())};
    const auto base = SDL_GetBasePath();
    if (base && std::filesystem::exists(path(base) / "unins000.exe"))
        args.push_back("/DIR=" + std::string(base));
#else
    std::vector<std::string> args{"pkexec", "/usr/bin/apt-get", "install", "--yes", utf8(file)};
#endif
    std::vector<const char *> pointers;
    for (const auto &arg : args)
        pointers.push_back(arg.c_str());
    pointers.push_back(nullptr);
    cancelled(stop);
    auto process = std::unique_ptr<SDL_Process, decltype(&SDL_DestroyProcess)>(
        SDL_CreateProcess(pointers.data(), false), SDL_DestroyProcess);
    if (!process) throw std::runtime_error("Could not start the update installer.");
#ifndef _WIN32
    int result{};
    while (!SDL_WaitProcess(process.get(), false, &result)) {
        cancelled(stop);
        SDL_Delay(50);
    }
    if (result != 0)
        throw std::runtime_error(
            "Installation was cancelled or failed. Try again and approve the system prompt.");
#endif
}
void relaunchUpdatedApp() {
#ifndef _WIN32
    const char *args[]{"/usr/bin/gw2-multi-launcher", nullptr};
    auto process = SDL_CreateProcess(args, false);
    if (!process) throw std::runtime_error("The update was installed. Reopen GW2 Multi Launcher.");
    SDL_DestroyProcess(process);
#endif
}
} // namespace gw2

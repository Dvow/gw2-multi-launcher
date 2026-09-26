module;
#include <array>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

export module update;
export import :common;
import platform;
import data_folder;

export namespace gw2 {
AppUpdate parseAppRelease(std::string_view body, std::string_view current = appVersion);
}

namespace gw2 {
bool appUpdatesEnabled() { return true; }
std::string_view appUpdateNotice() { return {}; }
AppUpdate parseAppRelease(std::string_view body, std::string_view current) {
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
    auto reply = https(
        "github.com", "/Dvow/gw2-multi-launcher/releases/latest/download/update.json", {}, {}, stop, 16384);
    if (reply.status != 200)
        throw std::runtime_error(
            "GitHub update check failed (HTTP " + std::to_string(reply.status) + "). Try again later.");
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
} // namespace gw2

module;
#include <array>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

export module update;

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
AppUpdate parseAppRelease(std::string_view body, std::string_view current = appVersion);
AppUpdate checkAppUpdate(std::stop_token stop);
std::filesystem::path downloadAppUpdate(const AppUpdate &release, std::stop_token stop);
void installAppUpdate(const AppUpdate &release, std::stop_token stop);
void relaunchUpdatedApp();
void clearParkedFiles();
} // namespace gw2

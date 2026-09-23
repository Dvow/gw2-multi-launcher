module;
#include <filesystem>
#include <array>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

export module epic;
import platform;

export namespace gw2 {
struct EpicSession {
    std::string identity, username;
    Secret record{}, access{};
};
class EpicConnection {
    std::array<std::string, 3> client_;
    Secret device_, authorization_;
    std::string url_;
    std::uint64_t next_{}, expires_{}, interval_{};

  public:
    EpicConnection(const std::filesystem::path &game, std::stop_token stop);
    const std::string &url() const { return url_; }
    std::optional<EpicSession> poll(std::stop_token stop);
};
EpicSession epicRefresh(const Secret &saved, const std::filesystem::path &game, std::stop_token stop);
} // namespace gw2

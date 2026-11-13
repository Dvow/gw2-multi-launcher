#pragma once
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gw2_proxy {
struct Settings {
    bool enabled{};
    std::string protocol{"socks5"}, host;
    unsigned port{1080};
    std::string username, protectedPassword;
    bool operator==(const Settings &) const = default;
};
inline void validate(const Settings &s) {
    if (s.protocol != "socks5" && s.protocol != "http" && s.protocol != "https")
        throw std::runtime_error("Choose SOCKS5, HTTP CONNECT, or HTTPS CONNECT for the proxy.");
    if (s.port == 0 || s.port > 65535)
        throw std::runtime_error("The proxy port must be between 1 and 65535.");
    const bool colon = s.host.find(':') != s.host.npos;
    if ((s.enabled && s.host.empty()) || s.host.size() > 253 ||
        s.host.find_first_of(" /\\@?#[]") != s.host.npos ||
        (colon && (std::ranges::count(s.host, ':') < 2 ||
            s.host.find_first_not_of("0123456789abcdefABCDEF:.") != s.host.npos)) ||
        std::ranges::any_of(s.host, [](unsigned char c) { return c <= 32 || c >= 127; }))
        throw std::runtime_error("Enter only the proxy hostname or IP address, without a URL or port.");
    if (s.username.size() > 255 || s.username.find(':') != s.username.npos ||
        std::ranges::any_of(s.username, [](unsigned char c) { return c < 32 || c == 127; }) ||
        s.protectedPassword.size() > 16384)
        throw std::runtime_error("The proxy username or saved password is invalid.");
}
inline void validatePassword(std::string_view password) {
    if (password.size() > 255 ||
        std::ranges::any_of(password, [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::runtime_error("Use a proxy password of up to 255 UTF-8 bytes without control characters.");
}
} // namespace gw2_proxy

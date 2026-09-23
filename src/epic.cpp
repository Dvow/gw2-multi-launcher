module;
#include <algorithm>
#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

module epic;
import game;
import platform;
import accounts;

namespace gw2 {
namespace {
std::uint64_t milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Response {
    nlohmann::json json;
    unsigned status{200};
    ~Response() {
        for (auto &value : json)
            if (value.is_string()) {
                auto &text = value.get_ref<std::string &>();
                wipe(text.data(), text.size());
            }
    }
    std::string_view field(const char *key) const {
        const auto found = json.find(key);
        if (found == json.end() || !found->is_string())
            throw std::runtime_error("Epic returned an incomplete sign-in response. Try signing in again.");
        const auto &text = found->get_ref<const std::string &>();
        if (text.empty() || text.size() > 8191 || text.find('\0') != text.npos)
            throw std::runtime_error("Epic returned an invalid sign-in response.");
        return text;
    }
    std::uint64_t seconds(const char *key, std::uint64_t maximum) const {
        const auto found = json.find(key);
        if (found == json.end() || !found->is_number_unsigned())
            throw std::runtime_error("Epic returned an invalid sign-in deadline.");
        const auto value = found->get<std::uint64_t>();
        if (!value || value > maximum) throw std::runtime_error("Epic returned an invalid sign-in deadline.");
        return value * 1000;
    }
    bool error(std::string_view code) const {
        const auto found = json.find("errorCode");
        return found != json.end() && found->is_string() && found->get_ref<const std::string &>() == code;
    }
    void check() const {
        if (status == 429) throw std::runtime_error("Epic is limiting sign-in requests. Try again later.");
        if (status >= 500) throw std::runtime_error("Epic is temporarily unavailable. Try again later.");
        if (status != 200 || json.contains("errorCode"))
            throw std::runtime_error("Epic rejected this sign-in. Reconnect the account in your browser.");
    }
};
Secret form(std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
    Secret result;
    std::size_t size{};
    for (const auto &[key, value] : fields)
        size += 3 * (key.size() + value.size()) + 2;
    result.bytes.reserve(size);
    const auto encode = [&](std::string_view text) {
        constexpr char hex[] = "0123456789ABCDEF";
        for (const unsigned char c : text) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                c == '_' || c == '.' || c == '~')
                result.bytes.push_back(c);
            else {
                result.bytes.push_back('%');
                result.bytes.push_back(hex[c >> 4]);
                result.bytes.push_back(hex[c & 15]);
            }
        }
    };
    for (const auto &[key, value] : fields) {
        if (!result.bytes.empty()) result.bytes.push_back('&');
        encode(key);
        result.bytes.push_back('=');
        encode(value);
    }
    return result;
}
Secret authorization(std::string_view kind, std::string_view value) {
    if (std::ranges::any_of(value, [](unsigned char c) { return c < 33 || c > 126; }))
        throw std::runtime_error("Epic returned an invalid authorization token.");
    Secret result;
    result.bytes.reserve(kind.size() + value.size());
    result.bytes.insert(result.bytes.end(), kind.begin(), kind.end());
    result.bytes.insert(result.bytes.end(), value.begin(), value.end());
    return result;
}
Secret basic(std::string_view value) {
    auto encoded = base64({reinterpret_cast<const unsigned char *>(value.data()), value.size()});
    auto result = authorization("Basic ", encoded);
    wipe(encoded.data(), encoded.size());
    return result;
}
Response request(std::string_view route, const Secret &auth, const Secret &body, std::stop_token stop) {
    auto reply = https("api.epicgames.dev", route, auth.text(), body.text(), stop);
    Response result{nlohmann::json::parse(reply.body.bytes, nullptr, false), reply.status};
    if (!result.json.is_object())
        throw std::runtime_error("Epic returned an unreadable response. Try again.");
    return result;
}
Response claims(std::string_view token) {
    const auto first = token.find('.'), last = token.rfind('.');
    if (first == token.npos || first == last || token.find('.', first + 1) != last)
        throw std::runtime_error("Epic returned an invalid account token.");
    Secret encoded;
    const auto payload = token.substr(first + 1, last - first - 1);
    encoded.bytes.reserve(payload.size() + 3);
    for (const auto c : payload)
        encoded.bytes.push_back(c == '-' ? '+' : c == '_' ? '/' : static_cast<unsigned char>(c));
    while (encoded.bytes.size() % 4)
        encoded.bytes.push_back('=');
    const auto decoded = unbase64(encoded.text());
    return {nlohmann::json::parse(decoded.bytes, nullptr, false)};
}
EpicSession session(
    const Response &response, const std::array<std::string, 3> &client, std::string_view expected = {}) {
    response.check();
    const auto identity = response.field("account_id");
    const auto refresh = response.field("refresh_token"), access = response.field("access_token");
    // Claims are read only from this HTTPS token response, never from browser input.
    // Bind the returned identity and game deployment before publishing or saving it.
    const auto token = claims(access);
    const auto username = token.field("dn");
    if (token.field("sub") != identity || token.field("aud") != client[0] ||
        token.field("pfdid") != client[2] || response.field("client_id") != client[0])
        throw std::runtime_error("Epic returned a token for a different account or game.");
    if (identity.size() != 32 ||
        !std::ranges::all_of(
            identity, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        username.size() > 256)
        throw std::runtime_error("Epic returned an invalid account identity.");
    if (!expected.empty() && identity != expected)
        throw std::runtime_error("Epic signed in to a different account. Reconnect this saved account.");
    EpicSession result{std::string(identity), std::string(username)};
    result.record.bytes.reserve(12 + identity.size() + username.size() + refresh.size());
    for (const auto value : {identity, username, refresh})
        textField(result.record.bytes, value);
    result.access.bytes.assign(access.begin(), access.end());
    return result;
}
} // namespace
EpicConnection::EpicConnection(const std::filesystem::path &game, std::stop_token stop)
    : client_(Image(game).epicClient()), authorization_(basic(client_[0] + ":" + client_[1])) {
    const auto started = milliseconds();
    const auto response = request("/epic/oauth/v2/deviceAuthorization", authorization_,
        form({{"client_id", client_[0]}, {"scope", "basic_profile"}, {"prompt", "login"}}), stop);
    response.check();
    const auto device = response.field("device_code"), code = response.field("user_code");
    if (response.field("verification_uri") != "https://www.epicgames.com/activate" || code.size() < 4 ||
        code.size() > 16 ||
        !std::ranges::all_of(code, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }))
        throw std::runtime_error("Epic returned an invalid browser sign-in address.");
    const auto complete = "https://www.epicgames.com/activate?userCode=" + std::string(code);
    if (response.field("verification_uri_complete") != complete)
        throw std::runtime_error("Epic returned an invalid browser sign-in address.");
    url_ = "https://www.epicgames.com/id/login?prompt=login%20new_session&" +
        std::string(form({{"redirectUrl", complete}}).text());
    device_.bytes.assign(device.begin(), device.end());
    interval_ = response.seconds("interval", 60);
    expires_ = started + response.seconds("expires_in", 1800);
    next_ = milliseconds() + interval_;
}
std::optional<EpicSession> EpicConnection::poll(std::stop_token stop) {
    cancelled(stop);
    if (milliseconds() >= expires_)
        throw std::runtime_error("Epic sign-in expired. Select Sign in to Epic to try again.");
    if (milliseconds() < next_) return {};
    const auto response = request("/epic/oauth/v2/token", authorization_,
        form({{"grant_type", "device_code"}, {"device_code", device_.text()}, {"deployment_id", client_[2]}}),
        stop);
    if (response.status == 400 && response.error("errors.com.epicgames.account.oauth.slow_down"))
        interval_ += 5000;
    else if (response.status != 400 ||
        !response.error("errors.com.epicgames.account.oauth.authorization_pending"))
        return session(response, client_);
    next_ = milliseconds() + interval_;
    return {};
}
EpicSession epicRefresh(const Secret &saved, const std::filesystem::path &game, std::stop_token stop) {
    Fields fields(saved.bytes);
    const auto identity = fields.next();
    (void)fields.next();
    const auto refresh = fields.next();
    fields.end();
    const auto client = Image(game).epicClient();
    auto response = request("/epic/oauth/v2/token", basic(client[0] + ":" + client[1]),
        form({{"grant_type", "refresh_token"}, {"refresh_token", refresh}, {"deployment_id", client[2]}}),
        stop);
    return session(response, client, identity);
}
} // namespace gw2

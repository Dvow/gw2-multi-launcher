module;
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#endif

export module session;
import game;
import platform;
import accounts;

export namespace gw2 {
inline std::filesystem::path installed(std::string_view name) {
    const auto base = SDL_GetBasePath();
    if (!base) throw std::runtime_error("Cannot find the launcher installation.");
    return path(base) / path(name);
}
inline std::unique_ptr<Process> gameRunner(const Catalog &c) {
    const auto helper = installed("GW2MultiLauncher.Host.exe");
    if (!std::filesystem::exists(helper) ||
        !std::filesystem::exists(installed("GW2MultiLauncher.Native.dll")))
        throw std::runtime_error("The game helper is missing. Reinstall the complete launcher package.");
    std::vector<std::string> args{utf8(helper)};
    std::vector<std::pair<std::string, std::string>> env;
#ifndef _WIN32
    if (!path(c.prefix).is_absolute() || !std::filesystem::exists(path(c.prefix) / "system.reg"))
        throw std::runtime_error("Select your existing Wine prefix in Settings.");
    args.insert(args.begin(), c.runner);
    env = {{"WINEPREFIX", c.prefix}, {"WINEDEBUG", "-all"}};
    if (path(c.runner).filename() == "umu-run") {
        if (!std::filesystem::exists(path(c.proton) / "proton"))
            throw std::runtime_error("Select your installed Proton directory in Settings.");
        for (const std::string_view system : {"/usr/", "/bin/", "/sbin/", "/lib/", "/lib64/"})
            if (args[1].starts_with(system)) {
                args[1] = "/run/host" + args[1];
                break;
            }
        auto libraries = utf8(helper.parent_path()) + ":" + utf8(path(c.gamePath).parent_path()) + ":" +
            c.prefix + ":" + utf8(dataRoot());
        if (auto existing = SDL_getenv("STEAM_COMPAT_LIBRARY_PATHS"); existing && *existing)
            libraries += ":" + std::string(existing);
        env.insert(env.end(),
            {{"PROTONPATH", c.proton}, {"GAMEID", "0"}, {"PROTON_VERB", "runinprefix"}, {"UMU_LOG", "warn"},
                {"STEAM_COMPAT_LIBRARY_PATHS", libraries}});
    }
    return std::make_unique<Process>(args, env, true);
#else
    (void)c;
    return std::make_unique<Process>(args, env);
#endif
}

struct AuthMessage {
    unsigned kind{};
    Secret data;
};
class SteamConnection {
    Process process_;
    Secret outgoing_, incoming_;
    std::array<unsigned char, 4> header_{};
    std::size_t written_{}, received_{};

  public:
    SteamConnection()
        : process_({utf8(installed(
#ifdef _WIN32
                       "steam/GW2MultiLauncher.Steam.exe"
#else
                       "steam/GW2MultiLauncher.Steam"
#endif
                       ))},
              {}) {
    }
    void send(unsigned kind, std::span<const unsigned char> fields = {}) {
        if (!outgoing_.bytes.empty() || fields.size() > 65532)
            throw std::runtime_error("The Steam sign-in request is not ready.");
        outgoing_.bytes.reserve(8 + fields.size());
        appendNumber(outgoing_.bytes, static_cast<unsigned>(4 + fields.size()));
        appendNumber(outgoing_.bytes, kind);
        outgoing_.bytes.insert(outgoing_.bytes.end(), fields.begin(), fields.end());
    }
    std::optional<AuthMessage> poll() {
        process_.write(outgoing_, written_);
        if (incoming_.bytes.empty()) {
            received_ += process_.read(std::span(header_).subspan(received_));
            if (received_ != header_.size()) return {};
            const auto size = number(header_);
            if (size < 4 || size > 65536) throw std::runtime_error("Invalid Steam helper response.");
            incoming_.bytes.resize(size);
            received_ = 0;
        }
        received_ += process_.read(std::span(incoming_.bytes).subspan(received_));
        if (received_ != incoming_.bytes.size()) return {};
        const auto kind = number(incoming_.bytes);
        received_ = 0;
        // Keep secret fields in a wiping buffer. Views remain valid only while
        // this message is alive, and are never published to presentation state.
        return AuthMessage{kind, std::exchange(incoming_, Secret{})};
    }
};

inline void gameRequest(Bytes &out, const Catalog &catalog, const Account &account) {
    appendNumber(out, 0x34585747);
    appendNumber(out,
        account.provider == Provider::epic        ? 3u
            : account.provider == Provider::steam ? 2u
                                                  : 0u);
    wireString(out, catalog.gamePath);
    const auto args = arguments(account, catalog);
    appendNumber(out, static_cast<unsigned>(args.size()));
    for (const auto &arg : args)
        wireString(out, arg);
    appendNumber(out, catalog.hideLogin ? 1u : 0u);
}
inline void tokenCredentials(Bytes &out, std::string_view displayName, std::string_view token) {
    auto name = utf16(displayName), value = utf16(token);
    out.reserve(out.size() + 12 + name.bytes.size() + value.bytes.size());
    appendNumber(out, static_cast<unsigned>(8 + name.bytes.size() + value.bytes.size()));
    appendNumber(out, static_cast<unsigned>(name.bytes.size() / 2 - 1));
    appendNumber(out, static_cast<unsigned>(value.bytes.size() / 2 - 1));
    out.insert(out.end(), name.bytes.begin(), name.bytes.end());
    out.insert(out.end(), value.bytes.begin(), value.bytes.end());
}
inline Secret steamRequest(const Catalog &catalog, const Account &account, Secret credentials) {
    Secret request;
    auto &out = request.bytes;
    out.reserve(262144);
    appendNumber(out, 0); // Total length, set after construction.
    for (const auto *value : {&catalog.runner, &catalog.prefix, &catalog.proton, &catalog.gamePath})
        textField(out, *value);
    appendNumber(out, static_cast<unsigned>(credentials.bytes.size()));
    out.insert(out.end(), credentials.bytes.begin(), credentials.bytes.end());
    gameRequest(out, catalog, account);
    const auto size = static_cast<unsigned>(out.size() - 4);
    if (size > 262144) throw std::runtime_error("The launch request is too large.");
    for (unsigned i = 0; i < 4; ++i)
        out[i] = static_cast<unsigned char>(size >> (8 * i));
    return request;
}

// This broker owns both the Steam ticket and game helper. A disconnected UI
// cannot revoke a running game's ticket. The broker exits when its game exits.
namespace broker {
inline std::atomic<unsigned> control{1};
struct NameInput {
    std::mutex mutex;
    std::string value;
    bool invalid{};
};
// The stdin reader may remain blocked when main exits. Keep its bounded mailbox
// alive until process termination instead of destroying a mutex it still owns.
inline NameInput &nameInput() {
    static auto value = new NameInput;
    return *value;
}
inline void read(void *out, std::size_t length) {
    if (std::fread(out, 1, length, stdin) != length) throw std::runtime_error("Closed session input.");
}
inline unsigned readNumber() {
    unsigned value{};
    read(&value, 4);
    return value;
}
inline bool connected{true};
inline void report(std::span<const unsigned char> bytes) {
    if (connected &&
        (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size() || std::fflush(stdout)))
        connected = false;
}
inline void report(unsigned state, unsigned detail = 0) {
    const std::array<unsigned, 3> record{0x34585747, state, detail};
    report({reinterpret_cast<const unsigned char *>(record.data()), sizeof(record)});
}
inline void readControl() {
    for (int byte; (byte = std::fgetc(stdin)) != EOF;) {
        if (byte == 1)
            control.fetch_or(2);
        else if (byte == 4)
            control.fetch_or(4);
        else if (byte == 5)
            control.fetch_or(8);
        else if (byte == 3) {
            try {
                const auto length = readNumber();
                if (length < 3 || length > 27) throw std::runtime_error("Invalid display name.");
                std::string name(length, '\0');
                read(name.data(), length);
                if (!validDisplayName(name)) throw std::runtime_error("Invalid display name.");
                std::lock_guard lock(nameInput().mutex);
                if (!nameInput().value.empty()) throw std::runtime_error("Duplicate display name.");
                nameInput().value = std::move(name);
            } catch (...) {
                std::lock_guard lock(nameInput().mutex);
                nameInput().invalid = true;
                break;
            }
        } else
            break;
    }
    control.fetch_and(~1u);
}
} // namespace broker

class SteamSession {
    enum class Stage { identity, ticket, client, name, failed } stage{Stage::identity};
    Secret outgoing;
    std::string identity, displayName{"1"};
    std::unique_ptr<SteamConnection> steam;
    std::unique_ptr<Process> game;
    std::array<unsigned char, 12> record{};
    std::size_t written{}, received{};
    bool show{};
    std::uint64_t deadline{};

    void readRequest() {
        const auto size = broker::readNumber();
        if (size < 32 || size > 262144) throw std::runtime_error("Invalid session request.");
        Secret request(size);
        broker::read(request.bytes.data(), size);
        Fields fields(request.bytes);
        Catalog catalog;
        catalog.runner = fields.next();
        catalog.prefix = fields.next();
        catalog.proton = fields.next();
        catalog.gamePath = fields.next();
        auto offset = 16 + catalog.runner.size() + catalog.prefix.size() + catalog.proton.size() +
            catalog.gamePath.size();
        const auto secretSize = number(request.bytes, offset);
        offset += 4;
        if (secretSize > size - offset) throw std::runtime_error("Invalid session request.");
        const auto saved = std::span(request.bytes).subspan(offset, secretSize);
        Fields identityFields(saved);
        identity = identityFields.next();
        (void)identityFields.next();
        (void)identityFields.next();
        identityFields.end();
        offset += secretSize;
        if (size - offset < 20 || number(request.bytes, offset) != 0x34585747 ||
            number(request.bytes, offset + 4) != 2)
            throw std::runtime_error("Invalid session request.");
        steam = std::make_unique<SteamConnection>();
        steam->send(3, saved);
        outgoing.bytes.assign(request.bytes.begin() + offset, request.bytes.end());
        request.clear();
        game = gameRunner(catalog);
        std::thread(broker::readControl).detach();
        broker::report(2);
        deadline = SDL_GetTicks() + 100000;
    }
    void requestTicket() {
        steam->send(5);
        stage = Stage::ticket;
        deadline = SDL_GetTicks() + 30000;
    }
    void controls() {
        const auto control = broker::control.fetch_and(~14u);
        if (!(control & 1)) {
            show |= broker::connected;
            broker::connected = false;
        }
        show |= (control & 2) != 0;
        // Forward the exit request without releasing Steam's ticket; only the
        // game helper's exited status ends this session.
        if (control & 8) {
            outgoing.bytes.push_back(5);
            show = false;
        }
        if (control & 4) {
            if (stage != Stage::name || !outgoing.bytes.empty())
                throw std::runtime_error("No account setup to cancel.");
            outgoing.bytes.push_back(4);
            stage = Stage::client;
        }
        auto &input = broker::nameInput();
        std::lock_guard lock(input.mutex);
        if (input.invalid) throw std::runtime_error("Invalid display name request.");
        if (input.value.empty()) return;
        if (stage != Stage::name) throw std::runtime_error("No display name requested.");
        displayName = std::move(input.value);
        input.value.clear();
        requestTicket();
        broker::report(2);
    }
    void fail(unsigned code) {
        broker::report(6, code);
        stage = Stage::failed;
        show = true;
    }
    void ticket(std::string_view hex) {
        if (hex.empty() || hex.size() >= 600 || hex.size() % 2 || !std::ranges::all_of(hex, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
            throw std::runtime_error("Invalid Steam ticket.");
        outgoing.bytes.push_back(3);
        tokenCredentials(outgoing.bytes, displayName, hex);
        stage = Stage::client;
    }
    void receiveSteam(const AuthMessage &message) {
        Fields result(std::span(message.data.bytes).subspan(4));
        if (message.kind == 5) {
            (void)result.next();
            const auto error = result.next();
            result.end();
            unsigned detail{};
            const auto parsed = std::from_chars(error.data(), error.data() + error.size(), detail);
            fail(parsed.ec == std::errc{} && parsed.ptr == error.data() + error.size() && detail >= 1010 &&
                        detail <= 1014
                    ? detail
                    : 1010);
            return;
        }
        if (message.kind == 3 && stage == Stage::identity) {
            if (result.next() != identity) throw std::runtime_error("Unexpected Steam identity.");
            (void)result.next();
            (void)result.next();
            result.end();
            requestTicket();
            return;
        }
        if (message.kind != 4 || stage != Stage::ticket)
            throw std::runtime_error("Steam authentication failed.");
        const auto hex = result.next();
        result.end();
        ticket(hex);
    }
    void pollSteam() {
        if (stage == Stage::failed) return;
        try {
            if (auto message = steam->poll()) receiveSteam(*message);
        } catch (...) {
            fail(1010);
        }
    }
    bool pollGame() {
        if ((stage == Stage::identity || stage == Stage::ticket) && SDL_GetTicks() > deadline)
            throw std::runtime_error("Steam authentication timed out.");
        if (!outgoing.bytes.empty())
            game->write(outgoing, written);
        else if (show) {
            const unsigned char command = 1;
            game->allowForeground();
            if (game->write({&command, 1})) show = false;
        }
        received += game->read(std::span(record).subspan(received));
        if (received != record.size()) return true;
        received = 0;
        if (number(record) != 0x34585747) throw std::runtime_error("Invalid game helper status.");
        broker::report(record);
        if (number(record, 4) == 9 && stage != Stage::failed) {
            stage = Stage::name;
            show |= !broker::connected;
        }
        return number(record, 4) != 5;
    }

  public:
    int run() {
        readRequest();
        for (;;) {
            controls();
            pollSteam();
            if (!pollGame()) return 0;
            SDL_Delay(outgoing.bytes.empty() ? 20 : 10);
        }
    }
};

inline int steamSession() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        return SteamSession{}.run();
    } catch (...) {
        broker::report(6, 1010);
        return 1;
    }
}
} // namespace gw2

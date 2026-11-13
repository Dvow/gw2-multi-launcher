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
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include "linux_setup.hpp"
#endif

#ifndef GW2_HELPER_MAGIC
#define GW2_HELPER_MAGIC 0x374C4D47
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
inline std::string dllDirectories(const std::vector<std::string> &dlls) {
    std::string result;
#ifndef _WIN32
    for (const auto &dll : dlls) {
        const auto directory = utf8(path(dll).parent_path());
        if (directory.find(':') != directory.npos)
            throw std::runtime_error("DLL directories cannot contain ':' when using Wine / Proton.");
        result += ":" + directory;
    }
#else
    (void)dlls;
#endif
    return result;
}
// game: the executable this launch starts (gamePathFor).
inline std::unique_ptr<Process> gameRunner(const Catalog &c, [[maybe_unused]] const std::string &game,
    const std::string &libraryDirectories, bool proxy = false) {
    const auto helper = installed(GW2_PROGRAM_FILE ".Host.exe");
    if (!std::filesystem::exists(helper) ||
        !std::filesystem::exists(installed(GW2_PROGRAM_FILE ".Native.dll")))
        throw std::runtime_error("The game helper is missing. Reinstall the complete launcher package.");
    std::vector<std::string> args{utf8(helper)};
    std::vector<std::pair<std::string, std::string>> env;
#ifndef _WIN32
    const auto setupProblems = linux_setup::problems({game, c.runner, c.prefix, c.proton},
        linux_setup::environment("PATH"));
    if (!setupProblems.empty()) throw std::runtime_error(setupProblems.front());
    args.insert(args.begin(), c.runner);
    env = {{"WINEPREFIX", c.prefix}, {"WINEDEBUG", "-all"}};
    if (path(c.runner).filename() == "umu-run") {
        for (const std::string_view system : {"/usr/", "/bin/", "/sbin/", "/lib/", "/lib64/"})
            if (args[1].starts_with(system)) {
                args[1] = "/run/host" + args[1];
                break;
            }
        auto libraries = utf8(helper.parent_path()) + ":" + utf8(path(game).parent_path()) + ":" +
            c.prefix + ":" + utf8(dataRoot()) + libraryDirectories;
        if (auto existing = SDL_getenv("STEAM_COMPAT_LIBRARY_PATHS"); existing && *existing)
            libraries += ":" + std::string(existing);
        env.insert(env.end(),
            {{"PROTONPATH", c.proton}, {"GAMEID", "0"}, {"PROTON_VERB", "runinprefix"}, {"UMU_LOG", "warn"},
                {"STEAM_COMPAT_LIBRARY_PATHS", libraries}});
    }
    if (proxy) {
        const auto python = linux_setup::findExecutable("python3", linux_setup::environment("PATH"));
        const auto script = installed("kx_proxy_linux.py");
        if (python.empty() || !std::filesystem::is_regular_file(script))
            throw std::runtime_error("Linux proxy support requires Python 3 and the complete launcher package.");
        args.insert(args.begin(), {utf8(python), utf8(script), "--"});
    }
    return std::make_unique<Process>(args, env, true);
#else
    (void)proxy;
    (void)c;
    (void)libraryDirectories;
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
                       GW2_PROGRAM_FILE ".Steam.exe"
#else
                       GW2_PROGRAM_FILE ".Steam"
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

// An account without an ID is the normal launch: global settings, no credentials, and
// GW2's own visible sign-in (helper operation 4).
void proxyRequest(Bytes &out, const Catalog &catalog, const Account &account, std::stop_token stop) {
    if (!account.proxy.enabled) return;
#ifdef _WIN32
    auto executable = path(catalog.mitmdump);
    if (executable.empty()) {
        executable = installed("mitmdump.exe");
        if (!std::filesystem::is_regular_file(executable)) {
            std::wstring found(32768, L'\0');
            const auto n = SearchPathW(nullptr, L"mitmdump.exe", nullptr,
                static_cast<DWORD>(found.size()), found.data(), nullptr);
            if (!n || n >= found.size())
                throw std::runtime_error("Install mitmproxy and select mitmdump.exe in Settings before using an account proxy.");
            found.resize(n);
            executable = found;
        }
    }
    if (!executable.is_absolute() || !std::filesystem::is_regular_file(executable))
        throw std::runtime_error("The mitmdump executable is missing. Update its path in Settings.");
    const auto script = installed("kx_proxy.py");
    if (!std::filesystem::is_regular_file(script) || !std::filesystem::is_regular_file(installed("kx_proxy_pipe.py")))
        throw std::runtime_error("The proxy helper script is missing. Reinstall the complete launcher package.");
    wireString(out, utf8(executable));
    wireString(out, utf8(script));
#else
    const auto search = linux_setup::environment("PATH");
    for (const auto *dependency : {"python3", "bwrap", "ip", "cp"})
        if (linux_setup::findExecutable(dependency, search).empty())
            throw std::runtime_error("Linux proxies require Python 3, bubblewrap, iproute2 and coreutils.");
    const auto executable = linux_setup::findExecutable(
        catalog.mitmdump.empty() ? "mitmdump" : catalog.mitmdump, search);
    if (executable.empty())
        throw std::runtime_error("Install mitmproxy 12.2 or newer and select its Linux mitmdump executable in Settings.");
#endif
    Secret password;
    if (!account.proxy.username.empty() && !account.proxy.protectedPassword.empty()) {
        auto encrypted = unbase64(account.proxy.protectedPassword);
        password = crypt(encrypted.bytes, false, false, stop);
    }
    nlohmann::json config{{"protocol", account.proxy.protocol}, {"host", account.proxy.host},
        {"port", account.proxy.port}, {"username", account.proxy.username}, {"password", password.text()}};
#ifndef _WIN32
    config["magic"] = GW2_HELPER_MAGIC;
    config["account"] = account.id;
    config["state"] = utf8(dataRoot() / "proxy-prefixes");
    config["mitmdump"] = utf8(executable);
#endif
    struct Clean {
        nlohmann::json &config;
        std::string text;
        ~Clean() {
            auto &value = config["password"].get_ref<std::string &>();
            wipe(value.data(), value.size());
            wipe(text.data(), text.size());
        }
    } clean{config, {}};
    clean.text = config.dump();
    // Bound and reserve before copying any plaintext into the wiping request.
    if (clean.text.size() > 8192) throw std::runtime_error("The proxy settings are too large.");
    out.reserve(out.size() + 4 + clean.text.size());
    textField(out, clean.text);
}
inline void gameRequest(Bytes &out, const Catalog &catalog, const Account &account,
    const std::vector<std::string> &dlls, bool ownMumble, std::stop_token stop = {}) {
    const bool normal = account.id.empty();
    const auto &game = gamePathFor(catalog, account);
    // A WeGame account, and a WeGame install's normal launch, are started by WeGame's bridge
    // (helper operation 5), which signs in and writes the game's command line itself.
    const bool viaWeGame = account.provider == Provider::wegame || (normal && wegame(game));
#ifndef _WIN32
    proxyRequest(out, catalog, account, stop);
#endif
    appendNumber(out, GW2_HELPER_MAGIC);
    appendNumber(out,
#ifdef _WIN32
        (account.proxy.enabled ? 16u : 0u) + (viaWeGame ? 5u : normal ? 4u
#else
        (viaWeGame ? 5u : normal ? 4u
#endif
            : account.provider == Provider::epic  ? 3u
            : account.provider == Provider::steam ? 2u
                                                  : 0u));
    wireString(out, game);
    // The Chinese client's field holds only its executable (Store::settings).
    const auto args = viaWeGame
        ? std::vector<std::string>{}
        : arguments(account, catalog, ownMumble, !game.empty() && game == catalog.chinaGamePath);
    appendNumber(out, static_cast<unsigned>(args.size()));
    for (const auto &arg : args)
        wireString(out, arg);
    appendNumber(out, catalog.hideLogin && !normal && !viaWeGame ? 1u : 0u);
#ifndef _WIN32
    auto files = dlls;
    if (path(catalog.runner).filename() == "umu-run") {
        for (auto &dll : files) {
            for (const std::string_view root : {"/usr/", "/opt/", "/bin/", "/sbin/", "/lib/", "/lib64/"}) {
                if (!dll.starts_with(root)) continue;
                dll.insert(0, "/run/host");
                break;
            }
        }
    }
#else
    const auto &files = dlls;
#endif
    appendNumber(out, static_cast<unsigned>(files.size()));
    for (const auto &dll : files)
        wireString(out, dll);
    // Where the helper records when this account's game closes; none for the normal launch.
    wireString(out, normal ? std::string{} : utf8(playedFile(account.id)));
#ifdef _WIN32
    proxyRequest(out, catalog, account, stop);
#endif
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
inline Secret steamRequest(const Catalog &catalog, const Account &account, Secret credentials,
    const std::vector<std::string> &dlls, bool ownMumble, std::stop_token stop = {}) {
    Secret request;
    auto &out = request.bytes;
    out.reserve(262144);
    appendNumber(out, 0); // Total length, set after construction.
    for (const auto *value : {&catalog.runner, &catalog.prefix, &catalog.proton, &catalog.gamePath})
        textField(out, *value);
    textField(out, dllDirectories(dlls));
    appendNumber(out, static_cast<unsigned>(credentials.bytes.size()));
    out.insert(out.end(), credentials.bytes.begin(), credentials.bytes.end());
    gameRequest(out, catalog, account, dlls, ownMumble, stop);
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
    Secret code;
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
    const std::array<unsigned, 3> record{GW2_HELPER_MAGIC, state, detail};
    report({reinterpret_cast<const unsigned char *>(record.data()), sizeof(record)});
}
inline void readControl() {
    for (int byte; (byte = std::fgetc(stdin)) != EOF;) {
        if (byte == 1)
            control.fetch_or(2);
        else if (byte == 5)
            control.fetch_or(8);
        else if (byte == 3 || byte == 6) {
            try {
                const auto length = readNumber();
                if (byte == 6) {
                    if (length != 12 && length != 14) throw std::runtime_error("Invalid verification code.");
                    Secret code;
                    code.bytes.reserve(5 + length);
                    code.bytes.push_back(6);
                    appendNumber(code.bytes, length);
                    code.bytes.resize(5 + length);
                    read(code.bytes.data() + 5, length);
                    std::lock_guard lock(nameInput().mutex);
                    if (!nameInput().code.bytes.empty()) throw std::runtime_error("Duplicate verification code.");
                    nameInput().code = std::move(code);
                    continue;
                }
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

#ifdef _WIN32
// The Steam app on this PC. Signed in to the launched account, it lets the game start its own
// Steam at sign-in (Bridge startSteam): overlay, controller layout, Steam wallet. A launch starts
// it when it is closed and would sign itself in to this account, then holds the ticket until it
// has signed in.
namespace steam_app {
inline DWORD active(const char *name) {
    DWORD value{}, size = sizeof(value);
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", name, RRF_RT_REG_DWORD,
               nullptr, &value, &size) == ERROR_SUCCESS
        ? value
        : 0;
}
// The signed-in account's 32-bit id; 0 while Steam starts, waits at its sign-in, or is closed.
inline DWORD signedIn() {
    return active("ActiveUser");
}
inline bool running() {
    const auto pid = active("pid");
    if (!pid) return false;
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    wchar_t image[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
        QueryFullProcessImageNameW(process, 0, image, &size) &&
        !_wcsicmp(std::filesystem::path(image).filename().c_str(), L"steam.exe");
    CloseHandle(process);
    return alive;
}
inline std::string text(const char *name) {
    char value[1024]{};
    DWORD size = sizeof(value);
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", name, RRF_RT_REG_SZ, nullptr, value, &size) ==
            ERROR_SUCCESS
        ? value
        : "";
}
// Starts the Steam app in the tray when it is closed and signs itself in to this login (its
// remembered account). Never with another account: that would only sign Steam in elsewhere.
inline bool start(std::string_view login) {
    const auto remembered = text("AutoLoginUser");
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (login.empty() || !std::ranges::equal(remembered, login, {}, lower, lower) || running()) return false;
    auto exe = std::filesystem::path(text("SteamExe")).make_preferred();
    std::error_code error;
    if (exe.empty() || !std::filesystem::is_regular_file(exe, error)) return false;
    std::wstring command = L"\"" + exe.wstring() + L"\" -silent";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            exe.parent_path().c_str(), &startup, &info))
        return false;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}
} // namespace steam_app
#endif

class SteamSession {
    enum class Stage { identity, ticket, client, name, failed } stage{Stage::identity};
    Secret outgoing;
    std::string identity, displayName{"1"};
    std::unique_ptr<SteamConnection> steam;
    std::unique_ptr<Process> game;
    std::array<unsigned char, 12> record{};
    std::size_t written{}, received{};
    bool show{}, closing{}, closeSent{};
    std::uint64_t deadline{};
#ifdef _WIN32
    // Until then a ticket waits for the Steam app this launch started to sign in.
    std::uint64_t steamWait{};
    Secret heldTicket;
#endif

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
        const std::string directories(fields.next());
        auto offset = 20 + catalog.runner.size() + catalog.prefix.size() + catalog.proton.size() +
            catalog.gamePath.size() + directories.size();
        const auto secretSize = number(request.bytes, offset);
        offset += 4;
        if (secretSize > size - offset) throw std::runtime_error("Invalid session request.");
        const auto saved = std::span(request.bytes).subspan(offset, secretSize);
        Fields identityFields(saved);
        identity = identityFields.next();
        [[maybe_unused]] const std::string login(identityFields.next());
        (void)identityFields.next();
        identityFields.end();
#ifdef _WIN32
        // Starting Steam takes about as long as GW2's sign-in form; 45 s covers a slow start.
        if (steam_app::start(login)) steamWait = SDL_GetTicks() + 45000;
#endif
        offset += secretSize;
        auto gameOffset = offset;
        bool linuxProxy = false;
#ifndef _WIN32
        if (size - offset >= 4 && number(request.bytes, offset) != GW2_HELPER_MAGIC) {
            const auto proxySize = number(request.bytes, offset);
            if (!proxySize || proxySize > 8192 || proxySize > size - offset - 4)
                throw std::runtime_error("Invalid Linux proxy request.");
            gameOffset += 4 + proxySize;
            linuxProxy = true;
        }
#endif
        if (size - gameOffset < 20 || number(request.bytes, gameOffset) != GW2_HELPER_MAGIC ||
            (number(request.bytes, gameOffset + 4) != 2 && number(request.bytes, gameOffset + 4) != 18))
            throw std::runtime_error("Invalid session request.");
        steam = std::make_unique<SteamConnection>();
        steam->send(3, saved);
        outgoing.bytes.assign(request.bytes.begin() + offset, request.bytes.end());
        request.clear();
        game = gameRunner(catalog, catalog.gamePath, directories, linuxProxy);
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
        const auto control = broker::control.fetch_and(~10u);
        if (!(control & 1)) {
            show |= broker::connected;
            broker::connected = false;
        }
        show |= (control & 2) != 0;
        // Forward Kill without releasing Steam's ticket; only the
        // game helper's exited status ends this session.
        if (control & 8) closing = true;
        auto &input = broker::nameInput();
        std::lock_guard lock(input.mutex);
        if (closing) {
            show = false;
            input.code.clear();
            input.value.clear();
            return;
        }
        if (input.invalid) throw std::runtime_error("Invalid display name request.");
        if (!input.code.bytes.empty()) {
            if (stage != Stage::client || !outgoing.bytes.empty())
                throw std::runtime_error("The game is not ready for a verification code.");
            outgoing = std::move(input.code);
        }
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
        stage = Stage::client;
#ifdef _WIN32
        if (waitingForSteam()) {
            heldTicket.bytes.assign(hex.begin(), hex.end());
            return;
        }
#endif
        outgoing.bytes.push_back(3);
        tokenCredentials(outgoing.bytes, displayName, hex);
    }
#ifdef _WIN32
    bool waitingForSteam() const {
        return steamWait && !steam_app::signedIn() && SDL_GetTicks() < steamWait;
    }
    // Sends a held ticket once the Steam app signed in, or once it took too long: the game then
    // signs in without Steam's overlay, controller layout and wallet.
    void releaseTicket() {
        if (heldTicket.bytes.empty() || waitingForSteam()) return;
        outgoing.bytes.push_back(3);
        tokenCredentials(outgoing.bytes, displayName, heldTicket.text());
        heldTicket.clear();
    }
#endif
    void receiveSteam(const AuthMessage &message) {
        Fields result(std::span(message.data.bytes).subspan(4));
        if (message.kind == 5) {
            (void)result.next();
            const auto error = result.next();
            result.end();
            unsigned detail{};
            const auto parsed = std::from_chars(error.data(), error.data() + error.size(), detail);
            fail(parsed.ec == std::errc{} && parsed.ptr == error.data() + error.size() && detail >= 1010 &&
                        detail <= 1015
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
        if (closing || stage == Stage::failed) return;
        try {
            if (auto message = steam->poll()) receiveSteam(*message);
        } catch (...) {
            fail(1010);
        }
    }
    bool readGame() {
        for (unsigned i = 0; i < 16; ++i) {
            const auto count = game->read(std::span(record).subspan(received));
            if (!count) break;
            received += count;
            if (received != record.size()) continue;
            received = 0;
            if (number(record) != GW2_HELPER_MAGIC) throw std::runtime_error("Invalid game helper status.");
            if (number(record, 4) == 6 && number(record, 8) == 1008) closing = closeSent = false;
            broker::report(record);
            if (number(record, 4) == 5) return false;
            if (number(record, 4) == 9 && stage != Stage::failed) {
                stage = Stage::name;
                show |= !broker::connected && !closing;
            }
        }
        return true;
    }
    bool pollGame() {
        if (!readGame()) return false;
        if (!closing && (stage == Stage::identity || stage == Stage::ticket) && SDL_GetTicks() > deadline)
            fail(1015); // Steam never answered. Retain the helper so the account can still be killed.
        try {
            if (!outgoing.bytes.empty())
                game->write(outgoing, written);
            else if (closing && !closeSent) {
                const unsigned char command = 5;
                if (game->write({&command, 1})) closeSent = true;
            } else if (show) {
                const unsigned char command = 1;
                game->allowForeground();
                if (game->write({&command, 1})) show = false;
            }
        } catch (...) {
            if (closing && !readGame()) return false;
            throw;
        }
        return true;
    }

  public:
    int run() {
        readRequest();
        for (;;) {
            controls();
            pollSteam();
#ifdef _WIN32
            releaseTicket();
#endif
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

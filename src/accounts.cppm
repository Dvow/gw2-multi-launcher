module;
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include "proxy_settings.hpp"
#ifdef _WIN32
#include "setup_paths.hpp"
#else
#include "linux_setup.hpp"
#endif

export module accounts;
import game;
import platform;
import update;

export namespace gw2 {
using ProxySettings = gw2_proxy::Settings;
// Saved as numbers: append only.
enum class Provider { arenaNet, steam, epic, kongZhong, wegame };
// A login name and password typed into GW2's own login: ArenaNet, and on the China client
// KongZhong, whose logins need not be email addresses.
inline bool passwordLogin(Provider p) { return p == Provider::arenaNet || p == Provider::kongZhong; }
// A platform session token from the launcher's own Steam or Epic sign-in.
inline bool platformLogin(Provider p) { return p == Provider::steam || p == Provider::epic; }
// Accounts of the Chinese client.
inline bool chinaProvider(Provider p) { return p == Provider::kongZhong || p == Provider::wegame; }
// WeGame saves nothing: WeGame's bridge signs in the account WeGame is signed in to.
inline constexpr unsigned maxProgramDelay = 600;
struct Program {
    std::string path;
    unsigned delay{}; // Seconds to wait after the previous program, or after the launch for the first.
    bool operator==(const Program &) const = default;
};
struct Account {
    std::string id, label, protectedCredentials, arguments;
    Provider provider{};
    std::string identity{}, username{};
    std::vector<std::string> dlls{};
    std::vector<Program> programs{}; // Started with this account, after the global ones.
    ProxySettings proxy{};
};
struct WindowState {
    int width{360}, height{480}, x{}, y{};
    bool positioned{};
    float scale{};
    bool operator==(const WindowState &) const = default;
};
struct Catalog {
    // gamePath is the global client, chinaGamePath the Chinese one (KongZhong or WeGame); a
    // player may have either or both. Each holds only its own client's executable.
    std::string gamePath, chinaGamePath, arguments, runner{"wine"}, prefix, proton;
    std::string updatePending; // The executable whose interrupted GW2 update must finish.
    bool hideLogin{true}, showPid{}, autoUpdate{}, autoUpdateAsked{}, alwaysOnTop{};
    bool hideGameRow{}; // Hide the home page's Guild Wars 2 row while nothing needs it.
    bool minimizeOnLaunch{}; // Minimize the launcher once a launch has its games running.
    WindowState window;
    std::vector<Account> accounts;
    std::vector<std::string> dlls;
    std::vector<Program> programs; // Started with every game launch unless already running.
    std::string mitmdump; // Optional absolute path; otherwise search the installation and PATH.
};
// The installation the home page's Guild Wars 2 row starts, sets up and repairs: the global
// client, or the Chinese one when it is the only one.
inline const std::string &normalGamePath(const Catalog &c) {
    return c.gamePath.empty() ? c.chinaGamePath : c.gamePath;
}
// The installation an account starts: KongZhong and WeGame accounts start the Chinese client,
// the others the global one. An account without an ID is the normal launch.
inline const std::string &gamePathFor(const Catalog &c, const Account &a) {
    return a.id.empty() ? normalGamePath(c) : chinaProvider(a.provider) ? c.chinaGamePath : c.gamePath;
}
// Why the normal launch's installation cannot launch, or empty when it can: a missing,
// incomplete, or wrong executable, or on Linux the Wine/Proton runner. Game updates and the
// native interface are checked later and keep their own messages.
inline std::string installationProblem(const Catalog &c) {
#ifdef _WIN32
    return setup_paths::gameProblem(path(normalGamePath(c)));
#else
    const auto issues = linux_setup::problems({normalGamePath(c), c.runner, c.prefix, c.proton},
        linux_setup::environment("PATH"));
    return issues.empty() ? std::string{} : issues.front();
#endif
}
// The WeGame edition of the China client: WeGame's bridge GW2WGLauncher.exe sits beside
// Gw2-64.exe, and only it can sign a WeGame account in. Its normal launch goes through it.
inline bool wegame(const std::string &gamePath) {
    std::error_code error;
    return !gamePath.empty() &&
        std::filesystem::is_regular_file(path(gamePath).parent_path() / "GW2WGLauncher.exe", error);
}
// Whether this Gw2-64.exe is the Chinese client, by the asset host it carries; false when
// it cannot be read.
inline bool chinaClient(const std::string &gamePath) {
    try {
        return Image(path(gamePath)).patchHost().china;
    } catch (const std::exception &) {
        return false;
    }
}
inline std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
    return first == value.npos ? "" : value.substr(first, last - first + 1);
}
inline std::string lower(std::string text) {
    for (auto &c : text)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}
inline void validateDlls(const std::vector<std::string> &dlls) {
    if (dlls.size() > 16) throw std::runtime_error("Choose at most 16 DLLs.");
    for (const auto &dll : dlls)
        if (dll.empty() || dll.size() >= 4096 || dll.find('\0') != dll.npos ||
            !path(dll).is_absolute() || lower(utf8(path(dll).extension())) != ".dll")
            throw std::runtime_error("Choose DLL files using full paths shorter than 4096 bytes.");
}
inline void validatePrograms(const std::vector<Program> &programs) {
    if (programs.size() > 16) throw std::runtime_error("Choose at most 16 programs.");
    for (const auto &[program, delay] : programs) {
        if (program.empty() || program.size() >= 4096 || program.find('\0') != program.npos ||
            !path(program).is_absolute() || lower(utf8(path(program).extension())) != ".exe")
            throw std::runtime_error("Choose programs (.exe) using full paths shorter than 4096 bytes.");
        if (delay > maxProgramDelay)
            throw std::runtime_error("Delay programs by at most " + std::to_string(maxProgramDelay) + " seconds.");
    }
}
inline std::vector<std::string> launchDlls(const Account &account, const Catalog &catalog) {
    std::vector<std::string> result;
    for (const auto *list : {&catalog.dlls, &account.dlls}) {
        validateDlls(*list);
        for (const auto &dll : *list) {
            std::error_code error;
            const auto file = std::filesystem::canonical(path(dll), error);
            if (error || !std::filesystem::is_regular_file(file, error))
                throw std::runtime_error("DLL is missing or unreadable: " + dll);
            if (std::ranges::none_of(result, [&](const auto &existing) {
                    return std::filesystem::equivalent(path(existing), file, error);
                }))
                result.push_back(utf8(file));
        }
    }
    validateDlls(result);
    return result;
}
// When each account's game last closed, shown on its row as "Last played": Unix seconds in
// "played/<account id>.txt" in the data folder. The game helper writes it when the game closes,
// also after the launcher closed (it waits for its game), and so does the launcher when it sees
// the close. One file per account keeps those writers off each other's accounts.
inline std::filesystem::path playedFile(std::string_view id) {
    const auto folder = dataRoot() / "played";
    std::filesystem::create_directories(folder);
    return folder / (std::string(id) + ".txt");
}
// The Unix time this account's game last closed, or 0 when none is known.
inline std::int64_t lastPlayed(std::string_view id) {
    std::ifstream input(playedFile(id));
    std::string text;
    std::int64_t value{};
    if (!std::getline(input, text) ||
        std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{})
        return 0;
    return std::max<std::int64_t>(value, 0);
}
inline void savePlayed(std::string_view id, std::int64_t time) {
    atomicWrite(playedFile(id), std::to_string(time) + "\n");
}
struct Option {
    std::string name;
    std::vector<std::string> values;
};
inline std::string optionWord(std::string_view text, std::size_t &i) {
    std::string word;
    bool quoted{};
    while (i < text.size() && (quoted || text[i] != ' ')) {
        std::size_t slashes{};
        while (i < text.size() && text[i] == '\\') {
            ++slashes;
            ++i;
        }
        if (i < text.size() && text[i] == '"') {
            word.append(slashes / 2, '\\');
            if (slashes % 2)
                word += '"';
            else if (quoted && i + 1 < text.size() && text[i + 1] == '"') {
                word += '"';
                ++i;
            } else
                quoted = !quoted;
            ++i;
        } else {
            word.append(slashes, '\\');
            if (i < text.size() && (quoted || text[i] != ' ')) word += text[i++];
        }
    }
    if (quoted) throw std::runtime_error("Close the quoted value in launch arguments.");
    return word;
}
inline std::string optionName(const std::string &word) {
    const auto start = word.find_first_not_of('-');
    auto name = start == word.npos ? std::string{} : lower(word.substr(start));
    if (name.empty() || name[0] < 'a' || name[0] > 'z' ||
        std::ranges::any_of(name, [](char c) { return !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9'); }))
        throw std::runtime_error("Use arguments such as -windowed or -loadmapinfo.");
    constexpr std::string_view reserved[] = {"email", "password", "password2", "autologin", "login",
        "authsrv", "authserver", "portal", "provider", "steam", "nopatchui", "dat", "localdat",
        "sharearchive", "webdisablecache", "image", "repair", "verify", "diag", "uninstall",
        "prefreset", "authlogin", "authpassword", "authtype", "epicapp", "epicenv", "epicportal",
        "epicusername", "epicuserid", "epiclocale", "epicsandboxid"};
    if (std::ranges::find(reserved, name) != std::end(reserved))
        throw std::runtime_error("-" + name + " is managed by the launcher or is a maintenance option.");
    return name;
}
inline std::vector<Option> options(std::string_view text) {
    if (text.size() > 2048 || std::ranges::any_of(text, [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::runtime_error("Keep launch arguments on one line, up to 2048 characters.");
    std::vector<Option> result;
    for (std::size_t i = 0; i < text.size();) {
        while (i < text.size() && text[i] == ' ')
            ++i;
        if (i == text.size()) break;
        auto word = optionWord(text, i);
        double numeric{};
        const auto parsed = std::from_chars(word.data(), word.data() + word.size(), numeric);
        if (word.size() > 1 && word[0] == '-' &&
            !(parsed.ec == std::errc{} && parsed.ptr == word.data() + word.size())) {
            result.push_back({optionName(word), {}});
        } else if (result.empty())
            throw std::runtime_error("Start arguments with a flag, such as -windowed.");
        else
            result.back().values.push_back(std::move(word));
    }
    // -mumble names the MumbleLink GW2 writes to; Nexus and other overlays read the same name.
    for (const auto &option : result) {
        if (option.name != "mumble") continue;
        if (option.values.size() != 1 || option.values[0].empty() || option.values[0].size() > 64 ||
            std::ranges::any_of(option.values[0], [](char c) {
                return !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
                    c != '_' && c != '-' && c != '.';
            }))
            throw std::runtime_error("Give -mumble one name of up to 64 letters, digits, - _ or ., "
                                     "such as -mumble MumbleLink2.");
    }
    return result;
}
// ownMumble: another client is already running. GW2 publishes its MumbleLink under the
// default name unless told otherwise, and Blish HUD and similar tools read only that name,
// so a lone client keeps it. Only a client started beside another gets its own link, so
// the two never overwrite each other. A -mumble in the player's own arguments always wins:
// a WeGame client takes no launcher arguments and keeps the default name, so a client
// started before it can only move off that name this way.
// china: the executable is the China (KongZhong) client (Image::patchHost).
inline std::vector<std::string> arguments(
    const Account &account, const Catalog &catalog, bool ownMumble, bool china) {
    auto combined = options(catalog.arguments);
    for (auto &option : options(account.arguments)) {
        std::erase_if(combined, [&](const Option &existing) { return existing.name == option.name; });
        combined.push_back(std::move(option));
    }
    const bool chosenMumble =
        std::ranges::any_of(combined, [](const Option &option) { return option.name == "mumble"; });
    auto id = account.id;
    std::erase(id, '-');
    std::vector<std::string> result{"-shareArchive", "-webdisablecache"};
    // The normal launch has no account ID and keeps GW2's default MumbleLink.
    if (ownMumble && !chosenMumble && !id.empty())
        result.insert(result.end(), {"-mumble", "GW2MultiLauncher-" + id});
    // The China client signs in through its own default provider, KongZhong: -provider Portal
    // sends a KongZhong account to ArenaNet's portal, which answers "wrong account or
    // password" (2026-10-23). Start it as KongZhong's own shortcut does.
    if (china)
        result.push_back("-StreamingClient");
    else if (account.provider == Provider::arenaNet)
        result.insert(result.end(), {"-provider", "Portal"});
    for (const auto &option : combined) {
        result.push_back("-" + option.name);
        result.insert(result.end(), option.values.begin(), option.values.end());
    }
    return result;
}
inline void appendNumber(Bytes &bytes, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<unsigned char>(n >> (i * 8)));
}
inline std::uint32_t number(std::span<const unsigned char> bytes, std::size_t offset = 0) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Invalid account record.");
    std::uint32_t result{};
    for (unsigned i = 0; i < 4; ++i)
        result |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8);
    return result;
}
inline void wireString(Bytes &bytes, std::string_view value) {
    auto text = utf16(value);
    const auto count = text.bytes.size() / 2 - 1;
    if (count > 32760) throw std::runtime_error("A path or argument is too long.");
    appendNumber(bytes, static_cast<std::uint32_t>(count));
    bytes.insert(bytes.end(), text.bytes.begin(), text.bytes.end() - 2);
}
inline bool validDisplayName(std::string_view value) {
    return value.size() >= 3 && value.size() <= 27 && value.front() != ' ' && value.back() != ' ' &&
        value.find("  ") == value.npos && std::ranges::all_of(value, [](char c) {
            return c == ' ' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        });
}
inline void textField(Bytes &bytes, std::string_view value) {
    if (value.size() > 32760 || value.find('\0') != value.npos)
        throw std::runtime_error("Invalid authentication field.");
    appendNumber(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
class Fields {
    std::span<const unsigned char> bytes_;

  public:
    explicit Fields(std::span<const unsigned char> bytes) : bytes_(bytes) {}
    std::string_view next() {
        const auto length = number(bytes_);
        bytes_ = bytes_.subspan(4);
        if (length > bytes_.size()) throw std::runtime_error("Invalid authentication record.");
        const auto value = std::string_view(reinterpret_cast<const char *>(bytes_.data()), length);
        bytes_ = bytes_.subspan(length);
        if (value.find('\0') != value.npos) throw std::runtime_error("Invalid authentication record.");
        return value;
    }
    void end() const {
        if (!bytes_.empty()) throw std::runtime_error("Invalid authentication record.");
    }
};
// email: an ArenaNet login must be an email address; a KongZhong one may be any login name.
inline Secret credentials(std::string_view email, std::span<const unsigned char> password, bool requireEmail = true) {
    auto address = utf16(email);
    const auto emailSize = address.bytes.size() / 2 - 1, passwordSize = password.size() / 2 - 1;
    if (emailSize < (requireEmail ? 3u : 1u) || emailSize >= 320 || (requireEmail && email.find('@') == email.npos) ||
        password.size() < 4 || password.size() % 2 || passwordSize >= 256 || password[password.size() - 1] ||
        password[password.size() - 2])
        throw std::runtime_error(requireEmail ? "Enter a valid email and a password of 1–255 characters."
                                              : "Enter your account and a password of 1–255 characters.");
    for (std::size_t i = 0; i + 2 < password.size(); i += 2)
        if (!password[i] && !password[i + 1])
            throw std::runtime_error("Passwords may not contain a null character.");
    Secret record;
    record.bytes.reserve(8 + address.bytes.size() + password.size());
    appendNumber(record.bytes, static_cast<std::uint32_t>(emailSize));
    appendNumber(record.bytes, static_cast<std::uint32_t>(passwordSize));
    record.bytes.insert(record.bytes.end(), address.bytes.begin(), address.bytes.end());
    record.bytes.insert(record.bytes.end(), password.begin(), password.end());
    return record;
}
inline Secret openAccount(const Account &account, std::stop_token stop) {
    if (account.provider == Provider::wegame) throw std::runtime_error("WeGame accounts save no login.");
    auto encrypted = unbase64(account.protectedCredentials);
    auto record = crypt(encrypted.bytes, false, false, stop);
    if (platformLogin(account.provider)) {
        Fields fields(record.bytes);
        const auto identity = fields.next(), username = fields.next(), token = fields.next();
        fields.end();
        if (identity != account.identity || username != account.username || token.empty() ||
            token.size() > 8192)
            throw std::runtime_error("This saved platform session is invalid. Sign in again.");
        return record;
    }
    auto email = number(record.bytes), password = number(record.bytes, 4);
    if (!email || email >= 320 || !password || password >= 256 ||
        record.bytes.size() != 8 + (email + password + 2) * 2 || record.bytes[8 + email * 2] ||
        record.bytes[9 + email * 2] || record.bytes[record.bytes.size() - 1] ||
        record.bytes[record.bytes.size() - 2])
        throw std::runtime_error("Invalid credential record.");
    for (std::size_t i = 8; i + 2 < record.bytes.size(); i += 2)
        if (i != 8 + email * 2 && !record.bytes[i] && !record.bytes[i + 1])
            throw std::runtime_error("Invalid credential record.");
    return record;
}

class Store {
    std::filesystem::path root_{catalogRoot()};
    FileLock lock_{root_ / "writer.lock"};
    Catalog catalog_;
    static void validate(const Catalog &c) {
        if (c.accounts.size() > 100 || c.gamePath.size() > 32760 || c.chinaGamePath.size() > 32760 ||
            c.updatePending.size() > 32760 || c.prefix.size() > 32760 ||
            c.proton.size() > 32760 || c.runner.size() > 32760)
            throw std::runtime_error("The account catalog is invalid.");
        (void)options(c.arguments);
        validateDlls(c.dlls);
        validatePrograms(c.programs);
        if (c.mitmdump.size() >= 4096 || c.mitmdump.find('\0') != c.mitmdump.npos ||
            (!c.mitmdump.empty() && !path(c.mitmdump).is_absolute()))
            throw std::runtime_error("Choose mitmdump using its full executable path.");
        std::set<std::string> ids;
        std::set<std::pair<Provider, std::string>> identities;
        for (const auto &a : c.accounts) {
            const auto id = lower(a.id);
            if (id.size() != 36 || id == "00000000-0000-0000-0000-000000000000" || id[8] != '-' ||
                id[13] != '-' || id[18] != '-' || id[23] != '-' || !ids.insert(id).second)
                throw std::runtime_error("The account catalog contains an invalid or duplicate account ID.");
            for (std::size_t i = 0; i < id.size(); ++i)
                if (i != 8 && i != 13 && i != 18 && i != 23 && !(id[i] >= '0' && id[i] <= '9') &&
                    !(id[i] >= 'a' && id[i] <= 'f'))
                    throw std::runtime_error("Invalid account ID.");
            if (static_cast<int>(a.provider) < 0 || a.provider > Provider::wegame)
                throw std::runtime_error("Unknown account provider. Update the launcher.");
            if ((a.provider == Provider::wegame) != a.protectedCredentials.empty() ||
                a.protectedCredentials.size() > 16384)
                throw std::runtime_error("The account catalog contains an invalid account.");
            if (platformLogin(a.provider) &&
                (a.identity.empty() || a.identity.size() > 64 || a.username.empty() ||
                    a.username.size() > 256))
                throw std::runtime_error("A saved platform identity is invalid.");
            if (platformLogin(a.provider) && !identities.emplace(a.provider, a.identity).second)
                throw std::runtime_error("This platform account is already saved.");
            // WeGame's bridge starts the game; a proxy needs the launcher to start it.
            if (a.provider == Provider::wegame && a.proxy.enabled)
                throw std::runtime_error("WeGame accounts cannot use a proxy.");
            (void)options(a.arguments);
            validateDlls(a.dlls);
            validatePrograms(a.programs);
            gw2_proxy::validate(a.proxy);
        }
    }
    Secret accountRecord(Account &updated, std::string_view id, std::string_view email, Secret replacement,
        Secret session, std::stop_token stop) {
        if (passwordLogin(updated.provider)) {
            Secret existing;
            if (replacement.bytes.empty() && !id.empty()) {
                // Anet and KongZhong records are alike: switching keeps the saved password.
                if (!passwordLogin(account(id).provider))
                    throw std::runtime_error("Enter this account's login and password.");
                existing = openAccount(account(id), stop);
                auto offset = 8 + (number(existing.bytes) + 1) * 2;
                replacement.bytes.assign(existing.bytes.begin() + offset, existing.bytes.end());
            }
            return credentials(email, replacement.bytes, updated.provider == Provider::arenaNet);
        }
        if (updated.provider == Provider::wegame) return Secret{};
        if (session.bytes.empty()) {
            if (id.empty() || account(id).provider != updated.provider)
                throw std::runtime_error("Sign in to this platform account before saving.");
            session = openAccount(account(id), stop);
        }
        Fields fields(session.bytes);
        updated.identity = fields.next();
        updated.username = fields.next();
        const auto token = fields.next();
        fields.end();
        if (token.empty() || token.size() > 8192)
            throw std::runtime_error("The platform session is invalid. Sign in again.");
        return session;
    }
    void save(Catalog next);
    // Linux creates its keyring entry with the first encrypted record; WeGame accounts have none.
    bool firstSecret() const {
        return std::ranges::all_of(catalog_.accounts, [](const Account &a) { return a.provider == Provider::wegame; });
    }

  public:
    Store();
    const Catalog &catalog() const { return catalog_; }
    const Account &account(std::string_view id) const {
        const auto found = std::ranges::find(catalog_.accounts, id, &Account::id);
        if (found == catalog_.accounts.end()) throw std::runtime_error("This account no longer exists.");
        return *found;
    }
    void settings(Catalog next) {
        next.accounts = catalog_.accounts;
        next.updatePending = catalog_.updatePending;
        next.window = catalog_.window;
        next.arguments = trim(next.arguments);
        next.mitmdump = trim(next.mitmdump);
        // Each executable is kept as its own client's, whichever field it was chosen in.
        std::string global, china;
        for (const auto *chosen : {&next.gamePath, &next.chinaGamePath}) {
            const auto value = trim(*chosen);
            if (value.empty()) continue;
            const auto file = path(value);
            if (!file.is_absolute() || lower(utf8(file.filename())) != "gw2-64.exe")
                throw std::runtime_error("Choose an installed Gw2-64.exe.");
            const Image image(file);
            (void)image.build();
            auto &slot = image.patchHost().china ? china : global;
            if (!slot.empty() && slot != value)
                throw std::runtime_error(&slot == &china ? "Both executables are the Chinese client. Choose one."
                                                         : "Both executables are the global client. Choose one.");
            slot = value;
        }
        next.gamePath = std::move(global);
        next.chinaGamePath = std::move(china);
#ifndef _WIN32
        next.runner = trim(next.runner);
        next.prefix = trim(next.prefix);
        next.proton = trim(next.proton);
        if (next.runner.empty())
            throw std::runtime_error("Choose wine, umu-run, or an absolute Wine runner path.");
        if (!next.prefix.empty() &&
            (!path(next.prefix).is_absolute() || !std::filesystem::exists(path(next.prefix) / "system.reg")))
            throw std::runtime_error("Choose an existing Wine prefix containing system.reg.");
        if (!next.proton.empty() && !std::filesystem::exists(path(next.proton) / "proton"))
            throw std::runtime_error("Choose your installed Proton directory.");
#else
        next.runner = catalog_.runner;
        next.prefix = catalog_.prefix;
        next.proton = catalog_.proton;
#endif
        save(std::move(next));
    }
    void put(std::string id, std::string label, std::string email, std::string args,
        std::vector<std::string> dlls, std::vector<Program> programs, Secret replacement, std::stop_token stop,
        Provider provider = Provider::arenaNet, Secret session = Secret{},
        std::optional<ProxySettings> proxy = {}) {
        if (!id.empty()) (void)account(id);
        label = trim(label);
        email = trim(email);
        args = trim(args);
        (void)options(args);
        std::erase(label, '\0');
        if (id.empty() && catalog_.accounts.size() >= 100)
            throw std::runtime_error("The account limit is 100.");
        Account updated{id.empty() ? identifier() : id, std::move(label), {}, std::move(args), provider};
        if (!id.empty()) updated.proxy = account(id).proxy;
        if (proxy) updated.proxy = std::move(*proxy);
        updated.dlls = std::move(dlls);
        validateDlls(updated.dlls);
        updated.programs = std::move(programs);
        validatePrograms(updated.programs);
        auto record = accountRecord(updated, id, email, std::move(replacement), std::move(session), stop);
        if (passwordLogin(updated.provider) && !email.empty())
            updated.username = email;
        if (updated.provider == Provider::wegame) updated.username = "WeGame";
        std::erase(updated.label, '\0');
        if (updated.provider != Provider::wegame) {
            auto encrypted = crypt(record.bytes, true, firstSecret(), stop);
            updated.protectedCredentials = base64(encrypted.bytes);
        }
        auto next = catalog_;
        if (id.empty())
            next.accounts.push_back(std::move(updated));
        else
            *std::ranges::find(next.accounts, id, &Account::id) = std::move(updated);
        cancelled(stop);
        save(std::move(next));
    }
    // Save proxy settings in the same transaction as the account. Plaintext is never
    // published to the UI snapshot, catalog, command line, or a temporary file.
    void putProxy(std::string id, std::string label, std::string email, std::string args,
        std::vector<std::string> dlls, std::vector<Program> programs, Secret replacement,
        std::stop_token stop, Provider provider, Secret session, ProxySettings proxy,
        Secret proxyPassword, bool clearPassword) {
        proxy.host = trim(proxy.host);
        proxy.protectedPassword = id.empty() ? "" : account(id).proxy.protectedPassword;
        // Username edits are autosaved independently of the password field. Keep
        // the sealed password until explicitly removed, including across restarts.
        if (clearPassword) proxy.protectedPassword.clear();
        if (!proxyPassword.bytes.empty()) {
            gw2_proxy::validatePassword(proxyPassword.text());
            if (proxy.username.empty())
                throw std::runtime_error("Enter a proxy username when setting a proxy password.");
            auto encrypted = crypt(proxyPassword.bytes, true, firstSecret(), stop);
            proxy.protectedPassword = base64(encrypted.bytes);
        }
        gw2_proxy::validate(proxy);
        // put() performs validation and encryption before the one atomic catalog write.
        put(std::move(id), std::move(label), std::move(email), std::move(args), std::move(dlls),
            std::move(programs), std::move(replacement), stop, provider, std::move(session), std::move(proxy));
    }
    void remove(std::string_view id) {
        (void)account(id);
        auto next = catalog_;
        std::erase_if(next.accounts, [&](const Account &a) { return a.id == id; });
        save(std::move(next));
    }
    void reorder(std::string_view id, std::string_view beforeId) {
        (void)account(id);
        if (!beforeId.empty()) (void)account(beforeId);
        const auto &list = catalog_.accounts;
        const auto from = std::ranges::find(list, id, &Account::id) - list.begin();
        const auto to =
            (beforeId.empty() ? list.end() : std::ranges::find(list, beforeId, &Account::id)) - list.begin();
        if (from == to || from + 1 == to) return;
        auto next = catalog_;
        const auto first = next.accounts.begin();
        if (from < to)
            std::rotate(first + from, first + from + 1, first + to);
        else
            std::rotate(first + to, first + from, first + from + 1);
        save(std::move(next));
    }
    void updatePending(std::string gamePath) {
        auto next = catalog_;
        next.updatePending = std::move(gamePath);
        save(std::move(next));
    }
    void automaticUpdates(bool enable) {
        auto next = catalog_;
        next.autoUpdate = enable;
        next.autoUpdateAsked = true;
        save(std::move(next));
    }
    void window(WindowState value) {
        if (value == catalog_.window) return;
        auto next = catalog_;
        next.window = value;
        save(std::move(next));
    }
};
std::uint32_t remoteBuild(std::string_view response);
} // namespace gw2

namespace gw2 {
// Older catalogs omit Proxy and remain direct by default.
nlohmann::json proxyJson(const ProxySettings &p) {
    return {{"Enabled", p.enabled}, {"Protocol", p.protocol}, {"Host", p.host}, {"Port", p.port},
        {"Username", p.username}, {"ProtectedPassword", p.protectedPassword}};
}
ProxySettings readProxy(const nlohmann::json &a) {
    if (!a.contains("Proxy")) return {};
    const auto &p = a.at("Proxy");
    if (!p.is_object()) throw std::runtime_error("The saved proxy settings are invalid.");
    ProxySettings result{p.value("Enabled", false), p.value("Protocol", "socks5"), p.value("Host", ""),
        p.value("Port", 1080u), p.value("Username", ""), p.value("ProtectedPassword", "")};
    gw2_proxy::validate(result);
    return result;
}
// A program without a delay is saved as its path alone, as before delays existed, so
// older launchers still read the catalog until a delay is set.
void to_json(nlohmann::json &json, const Program &program) {
    if (program.delay)
        json = {{"Path", program.path}, {"Delay", program.delay}};
    else
        json = program.path;
}
void from_json(const nlohmann::json &json, Program &program) {
    if (json.is_string())
        program = {json.get<std::string>()};
    else
        program = {json.at("Path").get<std::string>(), json.value("Delay", 0u)};
}
void Store::save(Catalog next) {
    validate(next);
    nlohmann::json accounts = nlohmann::json::array();
    for (const auto &a : next.accounts)
        accounts.push_back({{"Id", a.id}, {"Label", a.label},
            {"ProtectedCredentials", a.protectedCredentials}, {"Arguments", a.arguments},
            {"Provider", static_cast<int>(a.provider)}, {"Identity", a.identity}, {"Username", a.username},
            {"Dlls", a.dlls}, {"Programs", a.programs}, {"Proxy", proxyJson(a.proxy)}});
    // An older launcher must refuse a proxy-enabled catalog instead of ignoring
    // the new field and silently launching the accounts directly.
    const auto version = std::ranges::any_of(next.accounts, [](const Account &a) { return a.proxy.enabled; }) ? 4 : 3;
    // UpdatePending stays a flag for older launchers; UpdatePendingPath names the installation.
    nlohmann::json json{{"Version", version}, {"GamePath", next.gamePath}, {"ChinaGamePath", next.chinaGamePath},
        {"Accounts", accounts}, {"HideLogin", next.hideLogin}, {"ShowPid", next.showPid},
        {"AlwaysOnTop", next.alwaysOnTop}, {"HideGameRow", next.hideGameRow},
        {"MinimizeOnLaunch", next.minimizeOnLaunch}, {"Arguments", next.arguments}, {"Mitmdump", next.mitmdump},
        {"Runner", next.runner}, {"Prefix", next.prefix}, {"Proton", next.proton},
        {"UpdatePending", !next.updatePending.empty()}, {"UpdatePendingPath", next.updatePending},
        {"Dlls", next.dlls}, {"Programs", next.programs}};
    if (!appUpdatesAlwaysOn()) {
        json["AutoUpdate"] = next.autoUpdate;
        json["AutoUpdateAsked"] = next.autoUpdateAsked;
    }
    json["Window"] = {{"Width", next.window.width}, {"Height", next.window.height}, {"X", next.window.x},
        {"Y", next.window.y}, {"Positioned", next.window.positioned}, {"Scale", next.window.scale}};
    atomicWrite(root_ / "accounts.json", json.dump(2));
    catalog_ = std::move(next);
}

Store::Store() {
    catalog_.autoUpdate = catalog_.autoUpdateAsked = appUpdatesAlwaysOn();
    const auto file = root_ / "accounts.json";
    if (!std::filesystem::exists(file)) {
#ifndef _WIN32
        catalog_.runner.clear();
        catalog_.hideLogin = false;
#endif
        return;
    }
    if (std::filesystem::file_size(file) > 4 * 1024 * 1024)
        throw std::runtime_error("The account catalog is too large.");
    try {
        std::ifstream stream(file);
        const auto json = nlohmann::json::parse(stream);
        const auto version = json.at("Version").get<int>();
        if (version != 3 && version != 4)
            throw std::runtime_error("This account catalog version is unsupported.");
        catalog_.gamePath = json.at("GamePath").get<std::string>();
        catalog_.chinaGamePath = json.value("ChinaGamePath", "");
        catalog_.arguments = json.value("Arguments", "");
        catalog_.mitmdump = json.value("Mitmdump", "");
        catalog_.dlls = json.value("Dlls", std::vector<std::string>{});
        catalog_.programs = json.value("Programs", std::vector<Program>{});
        catalog_.hideLogin = json.value("HideLogin", true);
        catalog_.showPid = json.value("ShowPid", false);
        catalog_.alwaysOnTop = json.value("AlwaysOnTop", false);
        catalog_.hideGameRow = json.value("HideGameRow", false);
        catalog_.minimizeOnLaunch = json.value("MinimizeOnLaunch", false);
        catalog_.runner = json.value("Runner", "wine");
        catalog_.updatePending = json.value("UpdatePendingPath", "");
        if (catalog_.updatePending.empty() && json.value("UpdatePending", false))
            catalog_.updatePending = catalog_.gamePath; // Written before there were two installations.
        if (!appUpdatesAlwaysOn()) {
            catalog_.autoUpdate = json.value("AutoUpdate", false);
            catalog_.autoUpdateAsked = json.contains("AutoUpdateAsked")
                ? json.at("AutoUpdateAsked").get<bool>()
                : json.contains("AutoUpdate");
        }
        catalog_.prefix = json.value("Prefix", "");
        catalog_.proton = json.value("Proton", "");
        if (auto w = json.find("Window"); w != json.end() && w->is_object()) {
            catalog_.window = {std::clamp(w->value("Width", 360), 240, 8192),
                std::clamp(w->value("Height", 480), 160, 8192), std::clamp(w->value("X", 0), -100000, 100000),
                std::clamp(w->value("Y", 0), -100000, 100000), w->value("Positioned", false),
                std::clamp(w->value("Scale", 0.0f), 0.0f, 8.0f)};
        }
        for (const auto &a : json.at("Accounts"))
            catalog_.accounts.push_back({lower(a.at("Id").get<std::string>()),
                a.at("Label").get<std::string>(), a.at("ProtectedCredentials").get<std::string>(),
                a.value("Arguments", ""), static_cast<Provider>(a.value("Provider", 0)),
                a.value("Identity", ""), a.value("Username", ""),
                a.value("Dlls", std::vector<std::string>{}), a.value("Programs", std::vector<Program>{}), readProxy(a)});
        validate(catalog_);
        // Before the Chinese client had its own field, GamePath held whichever client the player
        // had, and accounts for the Chinese one were Anet accounts holding KongZhong logins.
        // Moved once; an executable that cannot be read now is checked again next start.
        if (!json.contains("ChinaGamePath") && !catalog_.gamePath.empty()) {
            try {
                if (Image(path(catalog_.gamePath)).patchHost().china) {
                    catalog_.chinaGamePath = std::exchange(catalog_.gamePath, {});
                    for (auto &account : catalog_.accounts)
                        if (account.provider == Provider::arenaNet) account.provider = Provider::kongZhong;
                }
                save(catalog_);
            } catch (const std::exception &) {
            }
        }
    } catch (const nlohmann::json::exception &) {
        throw std::runtime_error("The account catalog is corrupt. Restore accounts.json from your backup.");
    }
}
// The asset host's record: "<build> <exe file> <exe size> <manifest file> <manifest size>".
std::uint32_t remoteBuild(std::string_view response) {
    std::uint32_t build{};
    const auto end = response.data() + response.size();
    const auto parsed = std::from_chars(response.data(), end, build);
    if (parsed.ec != std::errc{} || parsed.ptr == end || *parsed.ptr != ' ')
        throw std::runtime_error("The GW2 update service returned an invalid build number.");
    return build;
}
} // namespace gw2

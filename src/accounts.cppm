module;
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

export module accounts;
import game;
import platform;

export namespace gw2 {
enum class Provider { arenaNet, steam, epic };
struct Account {
    std::string id, label, protectedCredentials, arguments;
    Provider provider{};
    std::string identity{}, username{};
    std::vector<std::string> dlls{};
};
struct WindowState {
    int width{360}, height{480}, x{}, y{};
    bool positioned{};
    float scale{};
    bool operator==(const WindowState &) const = default;
};
struct Catalog {
    std::string gamePath, arguments, runner{"wine"}, prefix, proton;
    bool hideLogin{true}, showPid{}, updatePending{}, autoUpdate{true}, alwaysOnTop{};
    WindowState window;
    std::vector<Account> accounts;
    std::vector<std::string> dlls;
};
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
        "sharearchive", "webdisablecache", "mumble", "image", "repair", "verify", "diag", "uninstall",
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
    return result;
}
inline std::vector<std::string> arguments(const Account &account, const Catalog &catalog) {
    auto combined = options(catalog.arguments);
    for (auto &option : options(account.arguments)) {
        std::erase_if(combined, [&](const Option &existing) { return existing.name == option.name; });
        combined.push_back(std::move(option));
    }
    auto id = account.id;
    std::erase(id, '-');
    std::vector<std::string> result{"-shareArchive", "-webdisablecache", "-mumble", "GW2MultiLauncher-" + id};
    if (account.provider == Provider::arenaNet) result.insert(result.end(), {"-provider", "Portal"});
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
inline Secret credentials(std::string_view email, std::span<const unsigned char> password) {
    auto address = utf16(email);
    const auto emailSize = address.bytes.size() / 2 - 1, passwordSize = password.size() / 2 - 1;
    if (emailSize < 3 || emailSize >= 320 || email.find('@') == email.npos || password.size() < 4 ||
        password.size() % 2 || passwordSize >= 256 || password[password.size() - 1] ||
        password[password.size() - 2])
        throw std::runtime_error("Enter a valid email and a password of 1–255 characters.");
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
    auto encrypted = unbase64(account.protectedCredentials);
    auto record = crypt(encrypted.bytes, false, false, stop);
    if (account.provider != Provider::arenaNet) {
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
    std::filesystem::path root_{dataRoot()};
    FileLock lock_{root_ / "writer.lock"};
    Catalog catalog_;
    static void validate(const Catalog &c) {
        if (c.accounts.size() > 100 || c.gamePath.size() > 32760 || c.prefix.size() > 32760 ||
            c.proton.size() > 32760 || c.runner.size() > 32760)
            throw std::runtime_error("The account catalog is invalid.");
        (void)options(c.arguments);
        validateDlls(c.dlls);
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
            if (trim(a.label).empty() || a.label.size() > 320 || a.protectedCredentials.empty() ||
                a.protectedCredentials.size() > 16384)
                throw std::runtime_error("The account catalog contains an invalid account.");
            if (a.provider != Provider::arenaNet && a.provider != Provider::steam &&
                a.provider != Provider::epic)
                throw std::runtime_error("Unknown account provider. Update the launcher.");
            if (a.provider != Provider::arenaNet &&
                (a.identity.empty() || a.identity.size() > 64 || a.username.empty() ||
                    a.username.size() > 256))
                throw std::runtime_error("A saved platform identity is invalid.");
            if (a.provider != Provider::arenaNet && !identities.emplace(a.provider, a.identity).second)
                throw std::runtime_error("This platform account is already saved.");
            (void)options(a.arguments);
            validateDlls(a.dlls);
        }
    }
    Secret accountRecord(Account &updated, std::string_view id, std::string_view email, Secret replacement,
        Secret session, std::stop_token stop) {
        if (updated.provider == Provider::arenaNet) {
            Secret existing;
            if (replacement.bytes.empty() && !id.empty()) {
                if (account(id).provider != Provider::arenaNet)
                    throw std::runtime_error("Enter the ArenaNet account's email and password.");
                existing = openAccount(account(id), stop);
                auto offset = 8 + (number(existing.bytes) + 1) * 2;
                replacement.bytes.assign(existing.bytes.begin() + offset, existing.bytes.end());
            }
            return credentials(email, replacement.bytes);
        }
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
        next.gamePath = trim(next.gamePath);
        next.arguments = trim(next.arguments);
        if (!next.gamePath.empty()) {
            auto file = path(next.gamePath);
            if (!file.is_absolute() || lower(utf8(file.filename())) != "gw2-64.exe")
                throw std::runtime_error("Choose an installed Gw2-64.exe.");
            (void)Image(file).build();
        }
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
        std::vector<std::string> dlls, Secret replacement, std::stop_token stop,
        Provider provider = Provider::arenaNet, Secret session = Secret{}) {
        if (!id.empty()) (void)account(id);
        label = trim(label);
        email = trim(email);
        args = trim(args);
        (void)options(args);
        auto name = utf16(label);
        if (label.empty() || name.bytes.size() / 2 > 81)
            throw std::runtime_error("Use an account name of 1–80 characters.");
        if (std::ranges::any_of(label, [](unsigned char c) { return c < 32 || c == 127; }))
            throw std::runtime_error("Use a single-line account name.");
        if (id.empty() && catalog_.accounts.size() >= 100)
            throw std::runtime_error("The account limit is 100.");
        Account updated{id.empty() ? identifier() : id, std::move(label), {}, std::move(args), provider};
        updated.dlls = std::move(dlls);
        validateDlls(updated.dlls);
        auto record = accountRecord(updated, id, email, std::move(replacement), std::move(session), stop);
        auto encrypted = crypt(record.bytes, true, catalog_.accounts.empty(), stop);
        updated.protectedCredentials = base64(encrypted.bytes);
        auto next = catalog_;
        if (id.empty())
            next.accounts.push_back(std::move(updated));
        else
            *std::ranges::find(next.accounts, id, &Account::id) = std::move(updated);
        cancelled(stop);
        save(std::move(next));
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
    void updatePending(bool value) {
        auto next = catalog_;
        next.updatePending = value;
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

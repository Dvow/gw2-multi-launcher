#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
import data_folder;
import platform;
import accounts;
import session;

namespace fs = std::filesystem;
using namespace gw2;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
Secret secret(std::string_view value) {
    Secret result;
    result.bytes.assign(value.begin(), value.end());
    return result;
}
std::string read(const fs::path &file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
void verify(const Store &store, const std::string &id, std::string_view expected, const fs::path &root) {
    const auto &account = store.account(id);
    if (expected.empty()) {
        require(account.proxy.protectedPassword.empty(), "Password was not explicitly cleared");
    } else {
        auto encrypted = unbase64(account.proxy.protectedPassword);
        auto opened = crypt(encrypted.bytes, false, false, {});
        require(opened.text() == expected, "Saved proxy password did not survive reopening");
        require(read(root / "accounts.json").find(expected) == std::string::npos,
            "Plaintext proxy password leaked into the catalog");
    }
    auto catalog = store.catalog();
    catalog.mitmdump = utf8(root / "mitmdump.exe");
    Secret request;
    proxyRequest(request.bytes, catalog, account, {});
    std::size_t offset{};
    for (int i = 0; i < 2; ++i) offset += 4 + number(request.bytes, offset) * 2;
    const auto size = number(request.bytes, offset);
    offset += 4;
    require(offset + size == request.bytes.size(), "Invalid proxy launch record size");
    const auto config = nlohmann::json::parse(request.bytes.begin() + offset, request.bytes.end());
    require(config.at("password").get<std::string>() == (account.proxy.username.empty() ? "" : expected),
        "Launch did not use the saved proxy password");
}
void verifyRestart(const fs::path &root, const std::string &id, std::string_view expected) {
    const auto executable = utf8(installed("proxy-storage-tests.exe"));
    const auto folder = utf8(root), password = std::string(expected);
    const char *args[]{executable.c_str(), "--verify-restart", folder.c_str(), id.c_str(), password.c_str(), nullptr};
    auto process = SDL_CreateProcess(args, false);
    require(process != nullptr, "Could not start the fresh-process persistence check");
    int code{};
    const bool exited = SDL_WaitProcess(process, true, &code);
    SDL_DestroyProcess(process);
    require(exited && code == 0, "Password failed the fresh-process persistence check");
}
// The launch serializes the player's -mumble ahead of the launcher's own link name.
void checkMumbleArguments() {
    const auto rejects = [](std::string_view text) {
        try {
            (void)options(text);
        } catch (const std::runtime_error &) { return true; }
        return false;
    };
    for (const auto text : {"-mumble", "-mumble \"\"", "-mumble a b", "-mumble Local\\x", "-mumble -windowed"})
        require(rejects(text), "An invalid -mumble was accepted");
    require(!rejects("-mumble MumbleLnk9 -windowed"), "A valid -mumble was rejected");
    const auto mumble = [](const std::vector<std::string> &args) {
        const auto found = std::ranges::find(args, "-mumble");
        return found == args.end() || std::next(found) == args.end() ? std::string{} : *std::next(found);
    };
    Account account{"0123abcd-0000-0000-0000-000000000001", "test", {}, {}, Provider::steam};
    Catalog catalog;
    require(mumble(arguments(account, catalog, false, false)).empty(), "A lone client left the default link");
    require(mumble(arguments(account, catalog, true, false)) == "GW2MultiLauncher-0123abcd000000000000000000000001",
        "A second client did not get its own link");
    catalog.arguments = "-mumble Shared";
    require(mumble(arguments(account, catalog, true, false)) == "Shared", "The global -mumble did not win");
    account.arguments = "-mumble MumbleLnk9";
    for (const bool second : {false, true}) {
        const auto args = arguments(account, catalog, second, false);
        require(mumble(args) == "MumbleLnk9" && std::ranges::count(args, "-mumble") == 1,
            "The account's -mumble did not replace every other link name");
    }
}
int main(int argc, char **argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--verify-restart") {
        gw2ConfigFolder() = argv[2];
        try {
            Store store;
            verify(store, argv[3], argv[4], path(argv[2]));
            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    const auto root = fs::temp_directory_path() / ("kx-proxy-storage-" + identifier());
    fs::create_directory(root);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{root};
    const auto rootName = utf8(root);
    gw2ConfigFolder() = rootName.c_str(); // Absolute test path; never opens the user's catalog.
    std::ofstream(root / "mitmdump.exe") << "test-only placeholder; never executed";
    try {
        checkMumbleArguments();
        std::string id, other;
        ProxySettings proxy{true, "socks5", "127.0.0.1", 1080, "test-user", {}};
        constexpr std::string_view first = "test-only proxy p@ss:with spaces!";
        constexpr std::string_view second = "replacement-test-password";
        {
            Store store;
            store.putProxy({}, "test", "test@example.invalid", {}, {}, {}, utf16("game-test-password"),
                {}, Provider::arenaNet, Secret{}, proxy, secret(first), false);
            id = store.catalog().accounts.front().id;
            verify(store, id, first, root);
            store.putProxy({}, "other", "other@example.invalid", {}, {}, {}, utf16("other-game-password"),
                {}, Provider::arenaNet, Secret{}, proxy, secret(second), false);
            other = store.catalog().accounts.back().id;
        }
        verifyRestart(root, id, first);
        verifyRestart(root, other, second);
        {
            Store reopened;
            verify(reopened, id, first, root);
            verify(reopened, other, second, root);
            // Clearing a username while replacing it must not silently delete the
            // independently saved password. No credentials are sent without a user.
            auto anonymous = proxy;
            anonymous.username.clear();
            reopened.putProxy(id, "test", "test@example.invalid", {}, {}, {}, Secret{}, {},
                Provider::arenaNet, Secret{}, anonymous, Secret{}, false);
            verify(reopened, id, first, root);
        }
        {
            Store reopened;
            verify(reopened, id, first, root);
            // A blank editor field restores authentication with the retained password.
            reopened.putProxy(id, "test", "test@example.invalid", {}, {}, {}, Secret{}, {},
                Provider::arenaNet, Secret{}, proxy, Secret{}, false);
            verify(reopened, id, first, root);
            auto disabled = proxy;
            disabled.enabled = false;
            reopened.putProxy(id, "test", "test@example.invalid", {}, {}, {}, Secret{}, {},
                Provider::arenaNet, Secret{}, disabled, Secret{}, false);
        }
        {
            Store reopened;
            reopened.putProxy(id, "test", "test@example.invalid", {}, {}, {}, Secret{}, {},
                Provider::arenaNet, Secret{}, proxy, Secret{}, false);
            verify(reopened, id, first, root);
            auto invalid = proxy;
            invalid.port = 0;
            bool rejected{};
            try {
                reopened.putProxy(id, "test", "test@example.invalid", {}, {}, {}, Secret{}, {},
                    Provider::arenaNet, Secret{}, invalid, secret(second), false);
            } catch (const std::runtime_error &) { rejected = true; }
            require(rejected, "Invalid edit was accepted");
            verify(reopened, id, first, root);
            // A blank editor field, nickname edit and repeated autosaves must keep the secret.
            for (int i = 0; i < 3; ++i)
                reopened.putProxy(id, "renamed", "test@example.invalid", {}, {}, {}, Secret{}, {},
                    Provider::arenaNet, Secret{}, proxy, Secret{}, false);
            auto settings = reopened.catalog();
            settings.showPid = !settings.showPid;
            reopened.settings(std::move(settings));
        }
        verifyRestart(root, id, first);
        {
            Store reopened;
            verify(reopened, id, first, root);
            verify(reopened, other, second, root);
            reopened.putProxy(id, "test", "test@example.invalid", {}, {}, {}, Secret{}, {},
                Provider::arenaNet, Secret{}, proxy, secret(second), false);
        }
        verifyRestart(root, id, second);
        {
            Store reopened;
            verify(reopened, id, second, root);
            reopened.putProxy(id, "test", "test@example.invalid", {}, {}, {}, Secret{}, {},
                Provider::arenaNet, Secret{}, proxy, Secret{}, true);
        }
        verifyRestart(root, id, {});
        {
            Store reopened;
            verify(reopened, id, {}, root);
            verify(reopened, other, second, root);
        }
        std::cout << "-mumble arguments, encrypted proxy save, reopen, blank edits, replacement, clear and launch passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

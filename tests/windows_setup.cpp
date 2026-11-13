#include "../src/windows_setup.hpp"
#include <iostream>
#include <stdexcept>

namespace setup = gw2::windows_setup;
namespace fs = std::filesystem;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void touch(const fs::path &path, const std::string &text = {}) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
void game(const fs::path &folder) { touch(folder / L"Gw2-64.exe"); touch(folder / L"Gw2.dat"); }
int main() {
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(fs::temp_directory_path().c_str(), L"gw2", 0, temporary)) return 1;
    DeleteFileW(temporary);
    fs::create_directory(temporary);
    // CI may supply an 8.3 temp path; discovery returns canonical paths.
    const fs::path root = fs::canonical(temporary);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); } } cleanup{root};
    try {
        setup::Environment env;
        require(setup::detect(env).games.empty(), "empty environment has no games");
        const auto standalone = root / L"Games/Guild Wars 2";
        env.gameFolders.push_back(root / L"Games");
        game(standalone);
        env.registeredGames = {standalone, standalone / L"Gw2-64.exe", standalone / L"GW2-64.EXE"};
        auto found = setup::detect(env);
        require(found.games.size() == 1, "registry and common paths deduplicate, including case");
        require(setup::defaultGame(found, {}) == standalone / L"Gw2-64.exe", "one install fills an empty path");
        const auto manual = root / L"Manual/Gw2-64.exe";
        game(manual.parent_path());
        require(setup::defaultGame(found, manual) == manual, "valid saved paths outside discovery are preserved");
        require(setup::defaultGame(found, root / L"missing/Gw2-64.exe") == standalone / L"Gw2-64.exe",
            "missing saved executable is repaired");
        touch(root / L"Manual/unrelated.exe");
        require(setup::defaultGame(found, root / L"Manual/unrelated.exe") == standalone / L"Gw2-64.exe",
            "a non-GW2 executable is not kept");
        const auto steam = root / L"Steam";
        const auto library = root / L"外部 Library";
        const auto steamGame = library / L"steamapps/common/Guild Wars 2";
        env.steamRoots.push_back(steam);
        const auto utf8 = library.u8string();
        std::string escaped;
        for (const auto c : utf8) { if (c == u8'\\') escaped += '\\'; escaped += static_cast<char>(c); }
        touch(steam / L"steamapps/libraryfolders.vdf", "\"libraryfolders\" { \"0\" { \"path\" \"" + escaped + "\" } }");
        game(steamGame);
        found = setup::detect(env);
        require(found.games.size() == 2, "external Steam library supports Unicode, spaces and escaped backslashes");
        require(found.games.back() == steamGame / L"Gw2-64.exe", "Steam path retains Unicode");
        require(setup::defaultGame(found, {}) == standalone / L"Gw2-64.exe", "prefer registered standalone over Steam");
        require(setup::defaultGame(found, steamGame / L"Gw2-64.exe") == steamGame / L"Gw2-64.exe",
            "valid saved Steam selection is preserved");
        {
            auto providers = env;
            const auto epic = root / L"外部 Epic Library/Custom GW2 Folder";
            const auto data = root / L"custom Epic data";
            providers.epicData = {data, L"relative/Epic"};
            providers.epicInstalled = root / L"Epic/LauncherInstalled.dat";
            game(epic);
            const auto name = epic.u8string();
            const nlohmann::json entry{{"InstallLocation", std::string(name.begin(), name.end())}};
            touch(data / L"Manifests/gw2.item", entry.dump());
            touch(data / L"Manifests/broken.item", "{invalid json");
            touch(data / L"Manifests/wrong-type.item", "{\"InstallLocation\":42}");
            touch(data / L"Manifests/relative.item", "{\"InstallLocation\":\"relative/GW2\"}");
            touch(providers.epicInstalled, nlohmann::json{{"InstallationList", {entry, nullptr, 42}}}.dump());
            auto all = setup::detect(providers);
            require(all.games.size() == 3 && all.games.back() == epic / L"Gw2-64.exe",
                "detect all three providers with custom Unicode Epic paths and no duplicate records");
            require(setup::defaultGame(all, epic / L"Gw2-64.exe") == epic / L"Gw2-64.exe",
                "preserve an Epic selection");
            fs::remove(data / L"Manifests/gw2.item");
            require(setup::detect(providers).games.size() == 3,
                "installed database finds Epic when its item manifest is missing");
            touch(providers.epicInstalled, "{invalid json");
            require(setup::detect(providers).games.size() == 2, "bad Epic records cannot stop other providers");
            touch(data / L"Manifests/gw2.item", entry.dump());
            require(setup::detect(providers).games.size() == 3, "item manifest works without the installed database");
            fs::remove(epic / L"Gw2.dat");
            require(setup::detect(providers).games.size() == 2, "Epic records require actual game files");
        }
        fs::remove(standalone / L"Gw2.dat");
        require(setup::defaultGame(found, standalone / L"Gw2-64.exe") == steamGame / L"Gw2-64.exe",
            "missing game data is repaired and stale candidates are skipped");
        require(setup::detect(env).games.size() == 1, "incomplete installations are excluded");
        const auto incomplete = setup::gameProblem(standalone / L"Gw2-64.exe");
        const auto moved = setup::gameProblem(root / L"missing/Gw2-64.exe");
        const auto wrong = setup::gameProblem(root / L"Manual/unrelated.exe");
        require(setup::gameProblem(manual).empty() && !incomplete.empty() && !moved.empty() && !wrong.empty() &&
                incomplete != moved && moved != wrong && wrong != incomplete && setup::gameProblem({}) != wrong,
            "missing, incomplete, and wrong executables each explain their problem");
        require(setup::defaultGame({}, root / L"missing/Gw2-64.exe") == root / L"missing/Gw2-64.exe",
            "no candidates leaves the saved path unchanged");
        env.registeredGames = {L"relative/Gw2-64.exe"};
        env.steamRoots = {L"relative/Steam"}; env.gameFolders = {L"relative/Games"};
        require(setup::detect(env).games.empty(), "relative paths never become candidates");
        require(setup::registryString(HKEY_CURRENT_USER, L"SOFTWARE\\GW2LauncherMissingTestKey", L"Path", 0).empty(),
            "missing registry keys are harmless");
        std::cout << "Windows installation discovery tests passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

#include "../src/linux_setup.hpp"
#include <iostream>
#include <stdexcept>

namespace setup = gw2::linux_setup;
namespace fs = std::filesystem;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void touch(const fs::path &path, bool executable = false, const std::string &text = {}) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
    if (executable) fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
}
void game(const fs::path &folder) { touch(folder / "Gw2-64.exe"); touch(folder / "Gw2.dat"); }
int main() {
    char pattern[] = "/tmp/gw2-setup-tests-XXXXXX";
    const auto created = ::mkdtemp(pattern);
    if (!created) return 1;
    const fs::path root = fs::canonical(created);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); } } cleanup{root};
    try {
        setup::Environment env{root / "home", root / "home/.local/share", (root / "bin").string(), {}, {}};
        fs::create_directories(env.home);
        require(setup::detect(env).games.empty(), "clean home must have no games");
        require(setup::defaults(setup::detect(env)).game.empty(), "no invented game paths");
        touch(root / "bin/umu-run", true); touch(root / "bin/wine", true);
        const auto library = root / "external disk/Steam Library";
        const auto steam = env.data / "Steam";
        touch(steam / "steamapps/libraryfolders.vdf", false, "\"libraryfolders\" { \"0\" { \"path\" \"" + library.string() + "\" } }");
        const auto install = library / "steamapps/common/Guild Wars 2";
        const auto prefix = library / "steamapps/compatdata/1284210/pfx";
        const auto proton = steam / "compatibilitytools.d/GE-Proton-Test";
        game(install); touch(prefix / "system.reg"); touch(proton / "proton", true);
        fs::create_directories(env.home / ".steam"); fs::create_directory_symlink(steam, env.home / ".steam/steam");
        auto found = setup::detect(env);
        auto selected = setup::defaults(found);
        require(found.games.size() == 1, "Steam symlinks must not duplicate installations");
        require(selected.game == (install / "Gw2-64.exe").string(), "external Steam library with spaces");
        require(selected.prefix == prefix.string(), "Steam game must use its app prefix");
        require(selected.runner == "umu-run", "prefer installed UMU");
        require(selected.proton == proton.string(), "one Proton may be selected");
        require(setup::problems(selected, env.searchPath).empty(), "complete setup passes validation");
        auto existing = selected;
        setup::fillMissing(existing, {"/different/game", "different-runner", "/different/prefix", "/different/proton"});
        require(existing.game == selected.game && existing.runner == selected.runner && existing.prefix == selected.prefix && existing.proton == selected.proton,
            "detection must preserve existing choices");
        touch(steam / "compatibilitytools.d/Other-Proton/proton", true);
        require(setup::defaults(setup::detect(env)).proton.empty(), "multiple Proton versions need a choice");
        const auto lutrisPrefix = root / "custom prefix";
        const auto lutrisGame = lutrisPrefix / "drive_c/Program Files/Guild Wars 2";
        touch(lutrisPrefix / "system.reg"); game(lutrisGame);
        touch(env.home / ".config/lutris/games/gw2.yml", false,
            "game:\n  exe: '" + (lutrisGame / "Gw2-64.exe").string() + "'\n  prefix: \"" + lutrisPrefix.string() + "\"\n");
        found = setup::detect(env);
        require(found.games.size() == 2 && setup::defaults(found).game == (install / "Gw2-64.exe").string(),
            "multiple games receive a stable automatic choice");
        require(found.games.back().prefix == lutrisPrefix, "Lutris YAML associates its prefix");
        fs::remove(prefix / "system.reg");
        require(setup::defaults(setup::detect(env)).game == (lutrisGame / "Gw2-64.exe").string(),
            "prefer a game with an existing prefix");
        touch(prefix / "system.reg");
        env.winePrefix = lutrisPrefix; env.protonPath = proton;
        const auto preferred = setup::detect(env);
        selected = setup::defaults(preferred);
        require(selected.game == (lutrisGame / "Gw2-64.exe").string(), "prefer the game matching WINEPREFIX");
        require(selected.prefix == lutrisPrefix && selected.proton == proton, "valid explicit environment preferences");
        const auto savedSteam = setup::defaults(preferred, install / "Gw2-64.exe");
        require(savedSteam.game == (install / "Gw2-64.exe").string() && savedSteam.prefix == prefix.string(),
            "saved game retains its matching prefix despite a different environment preference");
        setup::Selection repaired{(root / "missing/Gw2-64.exe").string(), "wine", prefix.string(), proton.string()};
        setup::fillMissing(repaired, selected);
        require(repaired.game == selected.game && repaired.prefix == selected.prefix &&
                repaired.runner == "wine" && repaired.proton == proton.string(),
            "repair a missing executable and its prefix without changing the runner or Proton");
        env.winePrefix = root / "missing"; env.protonPath = root / "missing";
        require(setup::defaults(setup::detect(env)).proton.empty(), "invalid environment paths are ignored");
        touch(root / "bin/not-executable");
        require(setup::findExecutable("not-executable", env.searchPath).empty(), "runner needs execute permission");
        require(setup::findExecutable("./umu-run", env.searchPath).empty(), "relative runner paths must not execute");
        require(setup::configPath("$(touch /tmp/pwned)", env.home).empty(), "config values must not execute shell");
        require(setup::configPath("~/Games/GW2", env.home) == env.home / "Games/GW2", "home expansion");
        const auto manual = root / "Manual/Gw2-64.exe";
        game(manual.parent_path());
        setup::Selection unrelated{manual.string(), "", "", ""};
        setup::fillMissing(unrelated, {(install / "Gw2-64.exe").string(), "umu-run", prefix.string(), proton.string()});
        require(unrelated.game == manual.string() && unrelated.prefix.empty(),
            "preserve a valid manual path without attaching another game's prefix");
        setup::Selection unavailable{(root / "missing/Gw2-64.exe").string(), "wine", prefix.string(), ""};
        const auto missing = unavailable.game;
        setup::fillMissing(unavailable, setup::defaults({}));
        require(unavailable.game == missing && unavailable.prefix == prefix.string(),
            "no candidates leaves saved paths unchanged");
        fs::remove(install / "Gw2.dat");
        require(setup::defaults(found, install / "Gw2-64.exe").game == (lutrisGame / "Gw2-64.exe").string(),
            "missing game data is repaired and stale candidates are skipped");
        require(setup::detect(env).games.size() == 1, "incomplete game folder is excluded");
        const auto issues = setup::problems({(install / "Gw2-64.exe").string(), "missing", "/missing", ""}, env.searchPath);
        require(issues.size() == 3, "report missing data, runner and prefix");
        std::cout << "Linux setup discovery and validation tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

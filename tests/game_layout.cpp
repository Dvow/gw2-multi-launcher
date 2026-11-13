// Resolves real game executables: every Gw2-64*.exe in GW2_TEST_GAME_EXES (a folder or one
// file). The newest build's Steam layout carries GW2's own Steam start; another layout, an
// older build (before 2026-09-18 GW2 called Steam's older SteamAPI_Init) or a build where that
// start changed still resolves without it. Run it on a new game build after a patch. Skipped
// without game executables; a build this launcher does not sign in to at all is left out.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
import game;

namespace {
void require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
bool hasSteam(const gw2::Layout &layout) {
    return layout.steamApiFile && layout.anchors[26].rva && layout.anchors[27].rva && layout.anchors[28].rva;
}
bool noSteam(const gw2::Layout &layout) {
    return !layout.steamApiFile && !layout.anchors[26].rva && !layout.anchors[27].rva && !layout.anchors[28].rva;
}
// A copy whose Steam start no longer matches: its call passes another app id.
std::filesystem::path withoutSteamStart(const std::filesystem::path &exe) {
    std::ifstream input(exe, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string id{'\xB9', '\x72', '\x98', '\x13', '\x00'};
    std::size_t changed{};
    for (auto at = std::string_view(bytes.data(), bytes.size()).find(id); at != std::string_view::npos;
         at = std::string_view(bytes.data(), bytes.size()).find(id, at + 1)) {
        bytes[at + 1] = '\x73';
        ++changed;
    }
    require(changed > 0, exe.string() + ": no Steam app id to change");
    const auto copy = std::filesystem::temp_directory_path() / "gw2-layout-test-no-steam.exe";
    std::ofstream(copy, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return copy;
}
} // namespace

int main() {
    std::string location;
    {
        char *value{};
        std::size_t size{};
        if (!_dupenv_s(&value, &size, "GW2_TEST_GAME_EXES") && value) location = value;
        std::free(value);
    }
    if (location.empty()) {
        std::cout << "GW2_TEST_GAME_EXES not set: skipped\n";
        return 77;
    }
    try {
        std::vector<std::filesystem::path> exes;
        if (std::filesystem::is_directory(location)) {
            for (const auto &entry : std::filesystem::directory_iterator(location)) {
                const auto name = entry.path().filename().string();
                if (entry.is_regular_file() && name.starts_with("Gw2-64") && entry.path().extension() == ".exe")
                    exes.push_back(entry.path());
            }
        } else {
            exes.push_back(location);
        }
        std::size_t supported{};
        std::filesystem::path newest;
        std::uint32_t newestTime{};
        bool newestSteam{};
        for (const auto &exe : exes) {
            const gw2::Image image(exe);
            gw2::Layout password;
            try {
                password = image.resolve(0);
            } catch (const std::runtime_error &) {
                std::cout << exe.filename().string() << ": not a build this launcher signs in to\n";
                continue;
            }
            require(noSteam(password), exe.string() + ": a password layout has no Steam start");
            const auto steam = image.resolve(1);
            require(hasSteam(steam) || noSteam(steam), exe.string() + ": a partial Steam start");
            std::cout << exe.filename().string() << (hasSteam(steam) ? ": Steam start found\n" : ": no Steam start\n");
            ++supported;
            if (password.timestamp >= newestTime) {
                newestTime = password.timestamp;
                newest = exe;
                newestSteam = hasSteam(steam);
            }
        }
        require(supported, "no Gw2-64*.exe in GW2_TEST_GAME_EXES that this launcher signs in to");
        require(newestSteam, newest.string() + ": the newest build's Steam layout has GW2's Steam start");
        const auto copy = withoutSteamStart(newest);
        struct Remove {
            std::filesystem::path path;
            ~Remove() {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } remove{copy};
        require(noSteam(gw2::Image(copy).resolve(1)), "a changed Steam start still resolves the Steam sign-in");
        std::cout << "Game layout tests passed (" << supported << " of " << exes.size() << " executables)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <exception>
#include <string_view>
import ui;
import session;
import update;

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--steam-session") return gw2::steamSession();
        if (argc != 1) return 2;
        gw2::clearParkedFiles();
        if (gw2::run() == 3) gw2::relaunchUpdatedApp();
        return 0;
    } catch (const std::exception &error) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GW2 Multi Launcher", error.what(), nullptr);
        return 1;
    }
}

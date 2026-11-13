module;
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

export module app_extension;

export namespace gw2 {
// What prepareLaunch did for one launch.
struct PreparedLaunch {
    bool managedDll{}; // It added its DLL ahead of the user's list.
    // Why the game starts without that DLL, when it should have had it. The launch goes on;
    // the app shows this on the Home page, where the player is looking.
    std::string problem;
};
// Installed before App starts. App owns these callbacks under the catalog's
// process lock and stops extension work before releasing that lock.
struct AppExtension {
    void (*start)(std::stop_token) = nullptr;
    void (*stop)() noexcept = nullptr;
    // Runs on a background thread for each launch with the user's DLLs and, on
    // Linux, the game's Wine prefix. May add a managed DLL ahead of the user's list
    // and report progress through the status callback (called on that thread).
    PreparedLaunch (*prepareLaunch)(std::vector<std::string> &, const std::string &, std::stop_token,
        const std::function<void(std::string)> &) = nullptr;
    // A game asked to restart for an add-on update (helper state 16): the next prepareLaunch
    // checks again instead of reusing a recent check. Called on the app's worker; must not block.
    void (*recheckLaunch)() noexcept = nullptr;
    // What the app calls the DLL prepareLaunch adds, while it loads and when it fails to.
    const char *managedName = "add-on";
};
AppExtension &appExtension() {
    static AppExtension extension;
    return extension;
}
}

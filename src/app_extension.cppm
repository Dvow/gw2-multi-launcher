module;
#include <stop_token>
#include <string>
#include <vector>

export module app_extension;

export namespace gw2 {
// Installed before App starts. App owns these callbacks under the catalog's
// process lock and stops extension work before releasing that lock.
struct AppExtension {
    void (*start)(std::stop_token) = nullptr;
    void (*stop)() noexcept = nullptr;
    void (*prepareLaunch)(std::vector<std::string> &, std::stop_token) = nullptr;
};
AppExtension &appExtension() {
    static AppExtension extension;
    return extension;
}
}

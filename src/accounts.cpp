module;
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

module accounts;
import game;
import platform;

namespace gw2 {
void Store::save(Catalog next) {
    validate(next);
    nlohmann::json accounts = nlohmann::json::array();
    for (const auto &a : next.accounts)
        accounts.push_back({{"Id", a.id}, {"Label", a.label},
            {"ProtectedCredentials", a.protectedCredentials}, {"Arguments", a.arguments},
            {"Provider", static_cast<int>(a.provider)}, {"Identity", a.identity}, {"Username", a.username},
            {"Dlls", a.dlls}});
    nlohmann::json json{{"Version", 3}, {"GamePath", next.gamePath}, {"Accounts", accounts},
        {"HideLogin", next.hideLogin}, {"ShowPid", next.showPid}, {"AlwaysOnTop", next.alwaysOnTop},
        {"Arguments", next.arguments},
        {"Runner", next.runner}, {"Prefix", next.prefix}, {"Proton", next.proton},
        {"UpdatePending", next.updatePending}, {"AutoUpdate", next.autoUpdate}, {"Dlls", next.dlls}};
    json["Window"] = {{"Width", next.window.width}, {"Height", next.window.height}, {"X", next.window.x},
        {"Y", next.window.y}, {"Positioned", next.window.positioned}, {"Scale", next.window.scale}};
    atomicWrite(root_ / "accounts.json", json.dump(2));
    catalog_ = std::move(next);
}

Store::Store() {
    const auto file = root_ / "accounts.json";
    if (!std::filesystem::exists(file)) return;
    if (std::filesystem::file_size(file) > 4 * 1024 * 1024)
        throw std::runtime_error("The account catalog is too large.");
    try {
        std::ifstream stream(file);
        const auto json = nlohmann::json::parse(stream);
        const auto version = json.at("Version").get<int>();
        if (version != 3)
            throw std::runtime_error("This account catalog version is unsupported.");
        catalog_.gamePath = json.at("GamePath").get<std::string>();
        catalog_.arguments = json.value("Arguments", "");
        catalog_.dlls = json.value("Dlls", std::vector<std::string>{});
        catalog_.hideLogin = json.value("HideLogin", true);
        catalog_.showPid = json.value("ShowPid", false);
        catalog_.alwaysOnTop = json.value("AlwaysOnTop", false);
        catalog_.runner = json.value("Runner", "wine");
        catalog_.updatePending = json.value("UpdatePending", false);
        catalog_.autoUpdate = json.value("AutoUpdate", true);
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
                a.value("Dlls", std::vector<std::string>{})});
        validate(catalog_);
    } catch (const nlohmann::json::exception &) {
        throw std::runtime_error("The account catalog is corrupt. Restore accounts.json from your backup.");
    }
}
std::uint32_t remoteBuild(std::string_view response) {
    try {
        return nlohmann::json::parse(response).at("id").get<std::uint32_t>();
    } catch (const nlohmann::json::exception &) {
        throw std::runtime_error("The GW2 update service returned an invalid build number.");
    }
}
} // namespace gw2

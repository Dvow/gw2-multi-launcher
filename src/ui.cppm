module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "font.hpp"
#ifdef _WIN32
#include "windows_setup.hpp"
#else
#include "linux_setup.hpp"
#endif
#include <chrono>
#include <future>
#include <optional>
#include "launch_recovery.hpp"

export module ui;
import platform;
import ui_surface;
import data_folder;
import accounts;
import app;
import update;
import ui_text;

namespace gw2::presentation {
#ifdef _WIN32
namespace launcher_setup = windows_setup;
#else
namespace launcher_setup = linux_setup;
#endif
const surface::Palette &ink() { return surface::active().palette; }
constexpr float iconSize = 28, iconSpacing = 4, headerHeight = 42, launcherMinWidth = 240;
int headerButtons() {
    return surface::active().panel.draw ? 4 : 3;
}
ImRect headerButton(int index, float width, float scale) {
    const auto margin = ImGui::GetStyle().WindowPadding.x;
    const auto button = iconSize * scale;
    const auto gap = iconSpacing * scale;
    const auto count = static_cast<float>(headerButtons());
    const ImVec2 pos{width - margin - count * button - (count - 1) * gap + index * (button + gap),
        (headerHeight - iconSize) * scale / 2};
    return {pos, {pos.x + button, pos.y + button}};
}
template <std::size_t N> void assign(std::array<char, N> &out, std::string_view text) {
    const auto size = std::min(N - 1, text.size());
    std::memcpy(out.data(), text.data(), size);
    out[size] = 0;
}
void mutedText(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ink().muted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}
void note(const std::string &text, ImVec4 color = ink().muted) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}
void heading(const char *text) {
    ImGui::PushFont(nullptr, 20);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}
void centerButtons(std::initializer_list<const char *> labels) {
    const auto pad = ImGui::GetStyle().FramePadding.x * 2;
    float row{};
    for (const auto *label : labels) {
        if (row) row += ImGui::GetStyle().ItemSpacing.x;
        row += ImGui::CalcTextSize(label).x + pad;
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (ImGui::GetContentRegionAvail().x - row) * 0.5f));
}
void centeredText(const char *text) {
    const auto avail = ImGui::GetContentRegionAvail().x;
    const auto width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail - width) * 0.5f));
    ImGui::TextUnformatted(text);
}
void help(const char *text) {
    if (!*text || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_NoSharedDelay |
            ImGuiHoveredFlags_AllowWhenDisabled))
        return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
bool button(const char *text, float width = 0, ImVec4 color = {}) {
    if (color.w) {
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImLerp(color, ImVec4{1, 1, 1, 1}, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImLerp(color, ImVec4{0, 0, 0, 1}, 0.14f));
    }
    const bool result = ImGui::Button(text, {width, iconSize * ImGui::GetStyle().FontScaleDpi});
    if (color.w) ImGui::PopStyleColor(3);
    return result;
}
enum class Icon {
    add,
    play,
    playAll,
    closeAll,
    edit,
    collapse,
    show,
    settings,
    back,
    minimize,
    close,
    check,
    remove,
    folder,
    password,
    qr,
    files
};
void drawIcon(Icon icon, ImVec2 origin, ImU32 color, float scale) {
    auto draw = ImGui::GetWindowDrawList();
    auto point = [&](ImVec2 p) { return ImVec2{origin.x + p.x * scale, origin.y + p.y * scale}; };
    auto stroke = [&](std::initializer_list<ImVec2> points, bool closed = false) {
        for (auto p : points)
            draw->PathLineTo(point(p));
        draw->PathStroke(color, closed ? ImDrawFlags_Closed : 0, 1.5f * scale);
    };
    auto circle = [&](ImVec2 center, float radius) {
        draw->AddCircle(point(center), radius * scale, color, 0, 1.5f * scale);
    };
    switch (icon) {
    case Icon::add:
        stroke({{8, 3}, {8, 13}});
        stroke({{3, 8}, {13, 8}});
        break;
    case Icon::play:
        draw->AddTriangleFilled(point({4, 2}), point({14, 8}), point({4, 14}), color);
        break;
    case Icon::playAll:
        draw->AddTriangleFilled(point({1, 3}), point({8, 8}), point({1, 13}), color);
        draw->AddTriangleFilled(point({8, 3}), point({15, 8}), point({8, 13}), color);
        break;
    case Icon::closeAll:
        stroke({{1, 11}, {1, 1}, {11, 1}});
        stroke({{4, 4}, {15, 4}, {15, 15}, {4, 15}}, true);
        stroke({{7, 7}, {12, 12}});
        stroke({{12, 7}, {7, 12}});
        break;
    case Icon::edit:
        stroke({{2, 10}, {10, 2}, {14, 6}, {6, 14}, {2, 14}}, true);
        stroke({{8, 4}, {12, 8}});
        break;
    case Icon::collapse:
        stroke({{3, 10}, {8, 5}, {13, 10}});
        break;
    case Icon::show:
        stroke({{1, 8}, {4, 4}, {8, 3}, {12, 4}, {15, 8}, {12, 12}, {8, 13}, {4, 12}}, true);
        circle({8, 8}, 2);
        break;
    case Icon::settings:
        stroke({{1, 4}, {4, 4}});
        stroke({{8, 4}, {15, 4}});
        stroke({{1, 12}, {8, 12}});
        stroke({{12, 12}, {15, 12}});
        circle({6, 4}, 2);
        circle({10, 12}, 2);
        break;
    case Icon::back:
        stroke({{7, 3}, {2, 8}, {7, 13}});
        stroke({{2, 8}, {14, 8}});
        break;
    case Icon::minimize:
        stroke({{3, 8}, {13, 8}});
        break;
    case Icon::close:
        stroke({{3, 3}, {13, 13}});
        stroke({{13, 3}, {3, 13}});
        break;
    case Icon::check:
        stroke({{2, 8}, {6, 12}, {14, 4}});
        break;
    case Icon::remove:
        stroke({{2, 4}, {14, 4}});
        stroke({{6, 4}, {6, 2}, {10, 2}, {10, 4}});
        stroke({{4, 4}, {5, 14}, {11, 14}, {12, 4}});
        break;
    case Icon::folder:
        stroke({{1, 5}, {1, 2}, {6, 2}, {8, 5}, {15, 5}, {13, 14}, {3, 14}, {1, 5}}, true);
        break;
    case Icon::password:
        circle({5, 6}, 3);
        stroke({{7, 8}, {13, 14}, {15, 12}});
        stroke({{10, 11}, {12, 9}});
        break;
    case Icon::qr:
        for (auto p : {ImVec2{1, 1}, ImVec2{10, 1}, ImVec2{1, 10}})
            draw->AddRect(point(p), point({p.x + 5, p.y + 5}), color, 0, 0, 1.5f * scale);
        stroke({{10, 10}, {10, 15}, {15, 15}, {15, 10}, {13, 10}});
        break;
    case Icon::files: {
        // Cloud: right bump, top bump, left bump, then the flat base.
        constexpr float pi = 3.14159265f;
        draw->PathArcTo(point({12, 10}), 3 * scale, pi / 2, -pi / 2);
        draw->PathArcTo(point({8, 7}), 4 * scale, 0, -pi);
        draw->PathArcTo(point({4, 10}), 3 * scale, -pi / 2, -3 * pi / 2);
        draw->PathStroke(color, ImDrawFlags_Closed, 1.5f * scale);
        break;
    }
    }
}
bool iconButton(Icon icon, const char *label, bool primary = false) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    auto color = primary ? ink().primary : ImVec4{};
    if (icon == Icon::play || icon == Icon::playAll) color = ink().launch;
    if (icon == Icon::close || icon == Icon::closeAll || icon == Icon::remove) color = ink().close;
    ImGui::PushID(label);
    const bool pressed = button("##action", iconSize * scale, color);
    ImGui::PopID();
    const auto pos = ImGui::GetItemRectMin();
    drawIcon(icon, {pos.x + 6 * scale, pos.y + 6 * scale}, ImGui::GetColorU32(ImGuiCol_Text), scale);
    help(label);
    return pressed;
}
void clippedText(const char *text, float width) {
    const auto pos = ImGui::GetCursorScreenPos();
    const ImVec2 end{pos.x + width, pos.y + ImGui::GetTextLineHeight()};
    ImGui::Dummy({width, end.y - pos.y});
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), pos, end, end.x, text, nullptr, nullptr);
    if (ImGui::CalcTextSize(text).x > width) help(text);
}
template <std::size_t N>
bool field(
    const char *name, std::array<char, N> &value, const char *hint = "", ImGuiInputTextFlags flags = 0) {
    mutedText(name);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(name);
    const bool changed = ImGui::InputTextWithHint("##value", hint, value.data(), value.size(), flags);
    ImGui::PopID();
    return changed;
}
void clearInputMemory() {
    auto &context = *ImGui::GetCurrentContext();
    ImGui::ClearActiveID();
    for (auto *text : {&context.InputTextState.TextA, &context.InputTextState.TextToRevertTo,
             &context.InputTextState.CallbackTextBackup, &context.InputTextDeactivatedState.TextA}) {
        wipe(text->Data, static_cast<std::size_t>(text->Capacity));
        text->clear();
    }
    context.InputTextState.ID = 0;
    context.InputTextState.TextLen = 0;
    context.InputTextDeactivatedState.ID = 0;
}
// ImGui owns additional editing buffers. Wipe freed allocations as well as our
// password field, including buffers replaced when a text input grows.
struct alignas(std::max_align_t) Allocation {
    std::size_t size;
};
void *allocate(std::size_t size, void *) {
    auto p = static_cast<Allocation *>(std::malloc(sizeof(Allocation) + size));
    if (!p) return nullptr;
    p->size = size;
    return p + 1;
}
void release(void *memory, void *) {
    if (!memory) return;
    auto p = static_cast<Allocation *>(memory) - 1;
    wipe(memory, p->size);
    std::free(p);
}

struct Picker {
    std::mutex mutex;
    bool done{};
    std::vector<std::string> values;
    std::string error;
};
// A game location being chosen: the Settings form's, or the home card's that first-run
// setup and launch repair share.
// A detected installation's executable.
#ifdef _WIN32
std::string detectedGame(const std::filesystem::path &game) { return utf8(game); }
#else
std::string detectedGame(const auto &game) { return game.game.string(); }
#endif
// Whether a chosen executable is there to launch; detection fills only fields that are not.
bool usableGame(const char *game) {
#ifdef _WIN32
    return setup_paths::gameExecutable(path(game));
#else
    std::error_code error;
    return *game && std::filesystem::is_regular_file(path(game), error);
#endif
}
struct Installation {
    // game is the global client's executable and china the Chinese one's. The home card
    // (home) edits only the normal launch's installation in game; saving keeps each
    // executable as its own client's (Store::settings, Action::installation).
    std::array<char, 32761> game{}, china{}, runner{}, prefix{}, proton{};
    bool home{};
    std::optional<launcher_setup::Detection> found;
    std::vector<std::string> chinaFound; // The detected executables that are the Chinese client.
    std::future<std::pair<launcher_setup::Detection, std::vector<std::string>>> detection;
    std::string detectionError;
    bool detected{}; // A detection finished that the home card has not looked at yet.
    bool settled{}; // The detection leaves no choice to make: at most one installation per client.
    Catalog fields() const {
        Catalog c;
        c.gamePath = game.data();
        if (!home) c.chinaGamePath = china.data();
        c.runner = runner.data();
        c.prefix = prefix.data();
        c.proton = proton.data();
        return c;
    }
    bool same(const Catalog &c) const {
        return (home ? normalGamePath(c) == game.data()
                     : c.gamePath == game.data() && c.chinaGamePath == china.data()) &&
            c.runner == runner.data() && c.prefix == prefix.data() && c.proton == proton.data();
    }
    void load(const Catalog &c, bool homeCard) {
        home = homeCard;
        assign(game, home ? normalGamePath(c) : c.gamePath);
        assign(china, home ? std::string{} : c.chinaGamePath);
        assign(runner, c.runner);
        assign(prefix, c.prefix);
        assign(proton, c.proton);
    }
    bool isChina(const std::string &executable) const {
        return std::ranges::find(chinaFound, executable) != chinaFound.end();
    }
    void detect() {
        if (detection.valid()) return;
        detectionError.clear();
        detection = std::async(std::launch::async, [] {
            auto found = launcher_setup::detect(launcher_setup::currentEnvironment());
            std::vector<std::string> china;
            for (const auto &entry : found.games)
                if (const auto game = detectedGame(entry); chinaClient(game)) china.push_back(game);
            return std::pair{std::move(found), std::move(china)};
        });
    }
    // Applies a finished detection and returns whether the draft changed. A single
    // installation of a client fills its unusable field; several need a deliberate choice.
    // The home card takes the global client, or the Chinese one when it is the only one.
    bool poll() {
        if (!detection.valid() || detection.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
        try {
            std::tie(found, chinaFound) = detection.get();
        } catch (const std::exception &) {
            detectionError = "Detection failed. Browse for Gw2-64.exe.";
            return false;
        }
        detected = true;
        const auto before = fields();
        std::vector<std::string> globals, chinas;
        for (const auto &entry : found->games) {
            auto detectedPath = detectedGame(entry);
            (isChina(detectedPath) ? chinas : globals).push_back(std::move(detectedPath));
        }
        // KongZhong's and WeGame's installations both found: WeGame's serves both account types
        // (started directly it signs in KongZhong accounts).
        if (chinas.size() > 1) {
            std::vector<std::string> wegames;
            for (const auto &chinese : chinas)
                if (wegame(chinese)) wegames.push_back(chinese);
            if (wegames.size() == 1) chinas = std::move(wegames);
        }
        settled = !found->games.empty() && globals.size() <= 1 && chinas.size() <= 1;
        if (!home && chinas.size() == 1 && !usableGame(china.data())) assign(china, chinas.front());
#ifdef _WIN32
        const auto &mine = home && globals.empty() ? chinas : globals;
        if (mine.size() == 1 && !usableGame(game.data())) assign(game, mine.front());
#else
        launcher_setup::Selection selection{game.data(), runner.data(), prefix.data(), proton.data()};
        auto suggested = launcher_setup::defaults(*found, path(selection.game));
        if (found->games.size() > 1) suggested.game.clear();
        launcher_setup::fillMissing(selection, suggested);
        assign(game, selection.game);
        assign(runner, selection.runner);
        assign(prefix, selection.prefix);
        assign(proton, selection.proton);
#endif
        return !same(before);
    }
};
// A file list entry's path: a DLL is only its path, a program also has a delay.
decltype(auto) filePath(auto &file) {
    if constexpr (requires { file.path; })
        return (file.path);
    else
        return (file);
}
struct Form {
    enum class Page { accounts, account, settings, exit } page{};
    struct Destination {
        Page page;
        std::string id;
    };
    std::variant<std::monostate, Destination, Command> next;
    double changedAt{-1};
    bool rowsMoving{}, scrollToNew{};
    std::vector<std::string> selected;
    std::string id, localError, notifiedUpdate;
    std::array<char, 1281> label{};
    std::array<char, 1281> email{};
    std::array<char, 1025> password{};
    std::array<char, 6> guard{};
    std::array<char, 7> verificationCode{};
    std::string verificationId;
    unsigned verificationPid{};
    bool focusVerification{};
    std::array<char, 28> displayName{};
    std::string displayNameId;
    Provider provider{};
    bool steamPassword{};
    App *authApp{};
    std::array<char, 2049> args{};
    std::vector<std::string> dlls;
    std::vector<Program> programs;
    ProxySettings proxy;
    std::array<char, 254> proxyHost{};
    std::array<char, 256> proxyUser{}, proxyPassword{};
    std::array<char, 4096> mitmdump{};
    bool clearProxyPassword{};
    // Picker targets 1-4 are installation paths (game, prefix, Proton, Chinese client); each
    // file list has an add target, then add+1+index replaces that entry.
    static constexpr unsigned dllPicker = 8, programPicker = 64;
    Installation install, setup; // Settings, and the home card's setup and repair.
    bool hide{}, showPid{}, alwaysOnTop{}, hideGameRow{}, minimizeOnLaunch{}, autoUpdate{}, askedAutoUpdate{},
        removing{}, loading{}, edited{}, connected{}, setupChecked{}, changing{}, scrollToSetup{};
    std::uint64_t pending{}, launchSerial{};
    Action pendingAction{};
    recovery::Request launchRequest;
    recovery::Recovery recovery; // A launch stopped by the installation, until repaired or cancelled.
    std::shared_ptr<Picker> picker;
    unsigned pickerField{};
    Installation *pickerInstall{};
    // Side panel width is in logical pixels. The toggle applies before the next frame lays out.
    bool panelOpen{}, togglePanel{}, panelFitted{}, panelFocused{};
    float panelWidth{};
    int panelAdded{}; // window pixels the open panel added; closing removes exactly these
    std::optional<std::pair<int, int>> panelShift; // window x before and after opening it moved the window
    ~Form() {
        wipe(proxyPassword.data(), proxyPassword.size());
        wipe(password.data(), password.size());
        wipe(verificationCode.data(), verificationCode.size());
    }
    void clearPassword() {
        wipe(password.data(), password.size());
        clearInputMemory();
    }
    void navigate(Page target) {
        if (page == target) return;
        if ((page == Page::settings) != (target == Page::settings)) changedAt = ImGui::GetTime();
        page = target;
    }
    bool animating() const { return rowsMoving || ImGui::GetTime() - changedAt < 0.16; }
    float opacity() const {
        const auto t = static_cast<float>(std::clamp((ImGui::GetTime() - changedAt) / 0.16, 0.0, 1.0));
        return 0.35f + 0.65f * t * t * (3 - 2 * t);
    }
    void close() {
        if (authApp) {
            authApp->submit({.action = Action::cancelConnect});
            authApp = nullptr;
        }
        rowsMoving = true;
        scrollToNew = false;
        clearPassword();
        wipe(proxyPassword.data(), proxyPassword.size());
        navigate(Page::accounts);
        removing = loading = edited = connected = false;
        id.clear();
        localError.clear();
    }
    void go(Page target, std::string accountId = {}) { next = Destination{target, std::move(accountId)}; }
    void request(Command command) { next = std::move(command); }
    void submit(App &app, Command command) {
        localError.clear();
        pendingAction = command.action;
        pending = app.submit(std::move(command));
    }
    void openSettings(const Catalog &c) {
        if (page == Page::settings) return;
        close();
        navigate(Page::settings);
        install.load(c, false);
        install.detect();
        assign(args, c.arguments);
        dlls = c.dlls;
        programs = c.programs;
        assign(mitmdump, c.mitmdump);
        hide = c.hideLogin;
        showPid = c.showPid;
        alwaysOnTop = c.alwaysOnTop;
        hideGameRow = c.hideGameRow;
        minimizeOnLaunch = c.minimizeOnLaunch;
        autoUpdate = c.autoUpdate;
    }
    void openAccount(const Account *account, App &app, const Catalog &catalog) {
        close();
        authApp = &app;
        // A new account starts as one of its installation's: KongZhong with only the Chinese client.
        provider = account ? account->provider
            : catalog.gamePath.empty() && !catalog.chinaGamePath.empty() ? Provider::kongZhong
                                                                         : Provider::arenaNet;
        steamPassword = false;
        guard.fill(0);
        navigate(Page::account);
        scrollToNew = !account;
        assign(label, account ? account->label : "");
        assign(email, "");
        assign(args, account ? account->arguments : "");
        dlls = account ? account->dlls : std::vector<std::string>{};
        programs = account ? account->programs : std::vector<Program>{};
        proxy = account ? account->proxy : ProxySettings{};
        assign(proxyHost, proxy.host);
        assign(proxyUser, proxy.username);
        clearProxyPassword = false;
        if (account) {
            id = account->id;
            loading = true;
            submit(app, {.action = Action::edit, .id = id});
        }
    }
    void observe(const Snapshot &snapshot) {
        if (snapshot.update.stage == UpdateStage::installed) go(Page::exit);
        const bool signedIn =
            snapshot.auth.connected && snapshot.auth.id == id && snapshot.auth.provider == provider;
        if (page == Page::account && !id.empty() && signedIn && !connected) edited = true;
        connected = signedIn;
        if (!verificationId.empty()) {
            const auto session = snapshot.session(verificationId);
            if (!session || !session->needsVerification() || session->pid != verificationPid) {
                wipe(verificationCode.data(), verificationCode.size());
                verificationId.clear();
                verificationPid = 0;
                focusVerification = false;
                clearInputMemory();
            }
        }
        if (!displayNameId.empty()) {
            const auto session = snapshot.session(displayNameId);
            if (!session || !session->active || session->state == 4) {
                displayName.fill(0);
                displayNameId.clear();
            }
        }
        const auto exists = [&](const std::string &id) {
            return std::ranges::find(snapshot.catalog.accounts, id, &Account::id) != snapshot.catalog.accounts.end();
        };
        std::erase_if(selected, [&](const auto &id) { return !exists(id); });
        // A launch stopped by the installation opens repair beside it, keeping its request.
        if (launchSerial && snapshot.completed >= launchSerial) {
            if (snapshot.error.selectGame) {
                recovery.stop(std::move(launchRequest));
                setup.load(snapshot.catalog, true);
                setup.detect();
                scrollToSetup = true;
            }
            launchSerial = 0;
        }
        if (!pending || snapshot.completed < pending) return;
        const auto done = std::exchange(pending, 0);
        loading = false;
        if (pendingAction == Action::installation) {
            changing &= !snapshot.error.message.empty();
            const bool stopped = recovery.open();
            resume(stopped, recovery.finished(done, !snapshot.error.message.empty(), exists));
        }
        if (!snapshot.error.message.empty()) {
            next = std::monostate{};
            if (pendingAction == Action::save || pendingAction == Action::settings ||
                pendingAction == Action::installation)
                localError = "Changes were not saved. " + snapshot.error.message;
            return;
        }
        if (pendingAction == Action::edit && snapshot.editId == id) assign(email, snapshot.editEmail);
        // Saving keeps each executable as its own client's: show where each one went.
        if (pendingAction == Action::settings && page == Page::settings) {
            assign(install.game, snapshot.catalog.gamePath);
            assign(install.china, snapshot.catalog.chinaGamePath);
        }
        if (pendingAction == Action::save) {
            clearPassword();
            wipe(proxyPassword.data(), proxyPassword.size());
            clearProxyPassword = false;
            const auto saved = std::ranges::find(snapshot.catalog.accounts, id, &Account::id);
            if (saved != snapshot.catalog.accounts.end()) assign(label, saved->label);
        }
        if ((pendingAction == Action::save && id.empty()) || pendingAction == Action::remove) close();
    }
    // Resubmits a repaired launch exactly as it was requested. A stopped launch that ended
    // without a request named a removed account, so nothing launches.
    void resume(bool stopped, std::optional<recovery::Request> request) {
        if (request)
            next = Command{.action = request->ids.empty() ? Action::launchGame : Action::launch,
                .ids = std::move(request->ids)};
        else if (stopped && !recovery.open())
            localError = "An account in this launch was removed, so nothing was launched.";
    }
    bool isSelected(const std::string &account) const {
        return std::ranges::find(selected, account) != selected.end();
    }
    void toggle(const std::string &account) {
        if (isSelected(account))
            std::erase(selected, account);
        else
            selected.push_back(account);
    }
    void launch(const Catalog &catalog) {
        Command command{.action = Action::launch, .ids = selected};
        if (selected.empty())
            for (const auto &account : catalog.accounts)
                command.ids.push_back(account.id);
        request(std::move(command));
    }
    void save(App &app, const Catalog &catalog) {
        Command c;
        if (page == Page::settings) {
            c.action = Action::settings;
            c.settings = catalog;
            c.settings.gamePath = install.game.data();
            c.settings.chinaGamePath = install.china.data();
            c.settings.arguments = args.data();
            c.settings.dlls = dlls;
            c.settings.programs = programs;
            c.settings.mitmdump = mitmdump.data();
            c.settings.hideLogin = hide;
            c.settings.showPid = showPid;
            c.settings.alwaysOnTop = alwaysOnTop;
            c.settings.hideGameRow = hideGameRow;
            c.settings.minimizeOnLaunch = minimizeOnLaunch;
            c.settings.autoUpdate = autoUpdate;
            c.settings.runner = install.runner.data();
            c.settings.prefix = install.prefix.data();
            c.settings.proton = install.proton.data();
        } else {
            c.action = Action::save;
            c.id = id;
            c.label = label.data();
            c.email = email.data();
            c.arguments = args.data();
            c.dlls = dlls;
            c.programs = programs;
            c.provider = provider;
            c.proxy = proxy;
            c.proxy.host = proxyHost.data();
            c.proxy.username = proxyUser.data();
            c.clearProxyPassword = clearProxyPassword;
            c.proxyPassword.bytes.assign(proxyPassword.data(), proxyPassword.data() + std::strlen(proxyPassword.data()));
            if (password[0]) c.password = utf16(password.data());
        }
        submit(app, std::move(c));
        edited = false;
    }
    bool canSave(const Snapshot &state) const {
        if (page == Page::settings) return true;
        if (state.auth.busy) return false;
        const auto account = std::ranges::find(state.catalog.accounts, id, &Account::id);
        if (account != state.catalog.accounts.end() && account->provider == provider) return true;
        if (provider == Provider::wegame) return true; // Nothing to sign in to here.
        // Switching between Anet and KongZhong keeps the saved password.
        if (passwordLogin(provider))
            return email[0] && (password[0] || (account != state.catalog.accounts.end() &&
                passwordLogin(account->provider)));
        return state.auth.connected && state.auth.id == id && state.auth.provider == provider;
    }
    void finish(App &app, const Snapshot &state, bool &open) {
        if (auto command = std::get_if<Command>(&next); command &&
            (command->action == Action::close || command->action == Action::closeAll ||
                command->action == Action::cancelSetup || command->action == Action::launch ||
                command->action == Action::launchGame)) {
            // Process termination must not wait for saving an unrelated form.
            auto intent = std::move(*command);
            next = std::monostate{};
            // Remember a launch until it is accepted, so an installation problem can resume it.
            const bool launching = intent.action == Action::launch || intent.action == Action::launchGame;
            if (launching) launchRequest = {intent.ids};
            const auto serial = app.submit(std::move(intent));
            if (launching) launchSerial = serial;
            return;
        }
        if (pending || picker) return;
        const bool requested = next.index() != 0;
        const auto pendingCommand = std::get_if<Command>(&next);
        const bool discard = pendingCommand &&
            (pendingCommand->action == Action::remove || pendingCommand->action == Action::cancelConnect);
        // Keep one user intent until the edited form has actually committed. A failed
        // save cancels that intent instead of navigating away from a rejected edit.
        if (edited && !discard && (page == Page::settings || (page == Page::account && !id.empty())) &&
            (!ImGui::IsAnyItemActive() || requested) && canSave(state)) {
            save(app, state.catalog);
            return;
        }
        auto intent = std::exchange(next, std::monostate{});
        if (auto command = std::get_if<Command>(&intent)) {
            submit(app, std::move(*command));
            return;
        }
        const auto destination = std::get_if<Destination>(&intent);
        if (!destination) return;
        if (destination->page == Page::settings) {
            openSettings(state.catalog);
            return;
        }
        if (destination->page == Page::account) {
            const auto account = std::ranges::find(state.catalog.accounts, destination->id, &Account::id);
            if (!destination->id.empty() && account == state.catalog.accounts.end()) return;
            openAccount(destination->id.empty() ? nullptr : &*account, app, state.catalog);
            return;
        }
        close();
        if (destination->page == Page::exit) open = false;
    }
    void choose(SDL_Window *window, unsigned target, bool folder, const char *initial,
        Installation *draft = nullptr) {
        picker = std::make_shared<Picker>();
        pickerField = target;
        pickerInstall = draft;
        auto data = new std::shared_ptr<Picker>(picker);
        auto callback = +[](void *opaque, const char *const *files, int) {
            std::unique_ptr<std::shared_ptr<Picker>> owner(static_cast<std::shared_ptr<Picker> *>(opaque));
            auto &reply = **owner;
            std::lock_guard lock(reply.mutex);
            if (!files)
                reply.error = "The file picker could not open. Try again.";
            else
                for (auto file = files; *file; ++file)
                    reply.values.emplace_back(*file);
            reply.done = true;
        };
        if (folder)
            SDL_ShowOpenFolderDialog(callback, data, window, initial, false);
        else if (target >= programPicker) {
            static constexpr SDL_DialogFileFilter filter{"Programs", "exe;EXE"};
            SDL_ShowOpenFileDialog(callback, data, window, &filter, 1, initial, target == programPicker);
        } else if (target >= dllPicker) {
            static constexpr SDL_DialogFileFilter filter{"DLL files", "dll;DLL"};
            SDL_ShowOpenFileDialog(callback, data, window, &filter, 1, initial, target == dllPicker);
        } else
            SDL_ShowOpenFileDialog(callback, data, window, nullptr, 0, initial, false);
    }
    void observePicker() {
        if (!picker) return;
        {
            std::lock_guard lock(picker->mutex);
            if (!picker->done) return;
            localError = picker->error;
            if (!picker->values.empty()) {
                try {
                    // A replaced program keeps its delay; added ones start without one.
                    const auto pick = [&](auto &files, unsigned add, auto validate) {
                        auto chosen = files;
                        if (pickerField > add)
                            filePath(chosen.at(pickerField - add - 1)) = picker->values.front();
                        else
                            for (const auto &file : picker->values)
                                if (std::ranges::none_of(chosen, [&](const auto &f) { return filePath(f) == file; }))
                                    chosen.push_back({file});
                        validate(chosen);
                        files = std::move(chosen);
                    };
                    if (pickerField >= programPicker)
                        pick(programs, programPicker, validatePrograms);
                    else if (pickerField >= dllPicker)
                        pick(dlls, dllPicker, validateDlls);
                    else {
                        auto &draft = *pickerInstall;
                        assign(pickerField == 1 ? draft.game
                                : pickerField == 2 ? draft.prefix
                                : pickerField == 3 ? draft.proton
                                                   : draft.china,
                            picker->values.front());
                    }
                    // The home card's draft is saved by its own buttons, never with these forms.
                    edited |= pickerField >= dllPicker || pickerInstall == &install;
                } catch (const std::exception &error) {
                    localError = error.what();
                }
            }
        }
        picker.reset();
    }
};

ImRect brandHitRect(float scale);
SDL_HitTestResult SDLCALL hitTest(SDL_Window *window, const SDL_Point *point, void *) {
    int w{}, h{};
    SDL_GetWindowSize(window, &w, &h);
    const auto scale = ImGui::GetCurrentContext() ? ImGui::GetStyle().FontScaleDpi : 1.0f;
    const int edge = static_cast<int>(5 * scale);
    if (point->y < edge)
        return point->x < edge     ? SDL_HITTEST_RESIZE_TOPLEFT
            : point->x >= w - edge ? SDL_HITTEST_RESIZE_TOPRIGHT
                                   : SDL_HITTEST_RESIZE_TOP;
    if (point->y >= h - edge)
        return point->x < edge     ? SDL_HITTEST_RESIZE_BOTTOMLEFT
            : point->x >= w - edge ? SDL_HITTEST_RESIZE_BOTTOMRIGHT
                                   : SDL_HITTEST_RESIZE_BOTTOM;
    if (point->x < edge) return SDL_HITTEST_RESIZE_LEFT;
    if (point->x >= w - edge) return SDL_HITTEST_RESIZE_RIGHT;
    if (point->y >= headerHeight * scale) return SDL_HITTEST_NORMAL;
    const ImVec2 position{static_cast<float>(point->x), static_cast<float>(point->y)};
    for (int i = 0; i < headerButtons(); ++i)
        if (headerButton(i, static_cast<float>(w), scale).Contains(position)) return SDL_HITTEST_NORMAL;
    if (brandHitRect(scale).Contains(position)) return SDL_HITTEST_NORMAL;
    return SDL_HITTEST_DRAGGABLE;
}
void publicStyle(ImGuiStyle &s) {
    ImGui::StyleColorsDark();
    s.WindowPadding = {8, 10};
    s.FramePadding = {9, 6};
    s.ItemSpacing = {8, 8};
    s.ItemInnerSpacing = {6, 4};
    s.WindowRounding = 0;
    s.FrameRounding = 5;
    s.ChildRounding = 7;
    s.PopupRounding = 6;
    s.ScrollbarSize = 6;
    s.ScrollbarRounding = 8;
    s.GrabMinSize = 18;
    s.WindowBorderSize = 1;
    s.HoverDelayShort = 0.3f;
    s.Colors[ImGuiCol_WindowBg] = ink().background;
    s.Colors[ImGuiCol_PopupBg] = {0.078f, 0.094f, 0.114f, 1};
    s.Colors[ImGuiCol_Border] = {0.16f, 0.19f, 0.22f, 1};
    s.Colors[ImGuiCol_FrameBg] = {0.095f, 0.114f, 0.137f, 1};
    s.Colors[ImGuiCol_TitleBg] = s.Colors[ImGuiCol_TitleBgActive] = s.Colors[ImGuiCol_FrameBg];
    s.Colors[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, 0.45f};
    s.Colors[ImGuiCol_FrameBgHovered] = {0.13f, 0.16f, 0.19f, 1};
    s.Colors[ImGuiCol_FrameBgActive] = {0.14f, 0.18f, 0.21f, 1};
    s.Colors[ImGuiCol_Text] = {0.90f, 0.93f, 0.95f, 1};
    s.Colors[ImGuiCol_TextDisabled] = ink().muted;
    s.Colors[ImGuiCol_Button] = {0.12f, 0.15f, 0.18f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.19f, 0.24f, 0.28f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {0.23f, 0.29f, 0.32f, 1};
    s.Colors[ImGuiCol_Header] = {0.18f, 0.20f, 0.22f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {0.24f, 0.26f, 0.29f, 1};
    s.Colors[ImGuiCol_HeaderActive] = ink().primary;
    s.Colors[ImGuiCol_CheckMark] = ink().accent;
    s.Colors[ImGuiCol_CheckboxSelectedBg] = s.Colors[ImGuiCol_FrameBgHovered];
    s.Colors[ImGuiCol_Separator] = s.Colors[ImGuiCol_Border];
    s.Colors[ImGuiCol_SeparatorHovered] = s.Colors[ImGuiCol_SeparatorActive] = ink().accent;
    s.Colors[ImGuiCol_ResizeGrip] = s.Colors[ImGuiCol_Border];
    s.Colors[ImGuiCol_ResizeGripHovered] = s.Colors[ImGuiCol_ResizeGripActive] = ink().accent;
    s.Colors[ImGuiCol_SliderGrab] = ink().primary;
    s.Colors[ImGuiCol_SliderGrabActive] = s.Colors[ImGuiCol_NavCursor] = ink().accent;
    s.Colors[ImGuiCol_TextSelectedBg] = {ink().accent.x, ink().accent.y, ink().accent.z, 0.35f};
    s.Colors[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    s.Colors[ImGuiCol_ScrollbarGrab] = {0.24f, 0.29f, 0.32f, 1};
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = ink().accent;
    s.Colors[ImGuiCol_ScrollbarGrabActive] = ink().primary;
}
void style(float scale) {
    auto &s = ImGui::GetStyle();
    if (const auto apply = surface::active().style) {
        apply(s);
        s.WindowRounding = 0;
        s.ChildRounding = 7;
        s.ChildBorderSize = 0;
        s.HoverDelayShort = 0.3f;
        s.FramePadding = {9, 6};
        s.ItemSpacing = {8, 8};
        s.ScrollbarSize = 6;
    } else {
        s = ImGuiStyle{};
        publicStyle(s);
    }
    s.SelectableRounding = 7;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

template <std::size_t N>
bool rowField(const char *name, std::array<char, N> &value, float width, const char *hint,
    ImGuiInputTextFlags flags = 0) {
    const auto line = ImGui::GetCursorScreenPos();
    const auto labelWidth = std::max(68 * ImGui::GetStyle().FontScaleDpi,
        ImGui::CalcTextSize("Arguments").x + ImGui::GetStyle().ItemSpacing.x);
    ImGui::AlignTextToFramePadding();
    mutedText(name);
    ImGui::SetCursorScreenPos({line.x + labelWidth, line.y});
    ImGui::SetNextItemWidth(width - labelWidth);
    ImGui::PushID(name);
    const bool changed = ImGui::InputTextWithHint("##value", hint, value.data(), value.size(), flags);
    ImGui::PopID();
    return changed;
}
// A program's delay, beside its tag and styled like one. Returns true when edited.
bool delayField(unsigned &delay, bool global) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    auto seconds = static_cast<int>(delay);
    ImGui::SameLine(0, 2 * scale);
    ImGui::BeginDisabled(global);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{5 * scale, 0});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("600 s").x + 10 * scale);
    const bool changed = ImGui::DragInt("##delay", &seconds, 0.25f, 0, static_cast<int>(maxProgramDelay), "%d s",
        ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    help("Seconds to wait before starting it, after the previous program. Drag, or double-click to type.");
    if (!changed || global) return false;
    delay = static_cast<unsigned>(seconds);
    return true;
}
// A row of file tags: inherited global files (disabled), then the form's own. Clicking
// a tag replaces that file, × removes it. Programs also show their delay. Picker
// targets: `add`, then add+1+index.
template <class File>
void fileField(Form &form, SDL_Window *window, float width, const char *name, const char *tip,
    std::vector<File> &files, const std::vector<File> &inherited, unsigned add,
    const char *empty, const char *choose) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto height = ImGui::GetFrameHeight();
    const auto labelWidth = std::max(68 * scale,
        ImGui::CalcTextSize("Arguments").x + ImGui::GetStyle().ItemSpacing.x);
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(name);
    ImGui::AlignTextToFramePadding();
    mutedText(name);
    help(tip);
    ImGui::SetCursorScreenPos({origin.x + labelWidth, origin.y});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{5 * scale, scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4 * scale);
    ImGui::BeginChild("##files", {std::max(1.f, width - labelWidth - 36 * scale), height},
        ImGuiChildFlags_FrameStyle, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::SetCursorPosY((height - ImGui::GetTextLineHeight()) * 0.5f);
    int removed = -1;
    auto tags = [&](auto &list, bool global) {
        ImGui::PushID(global);
        for (std::size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (i || (!global && !inherited.empty())) ImGui::SameLine();
            const auto &full = filePath(list[i]);
            const auto file = utf8(path(full).filename());
            ImGui::BeginDisabled(global);
            if (ImGui::SmallButton(file.c_str()))
                form.choose(window, add + 1 + static_cast<unsigned>(i), false, full.c_str());
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s%s", global ? "Global: " : "Replace: ", full.c_str());
            if constexpr (std::is_same_v<File, Program>) {
                auto delay = list[i].delay;
                if (delayField(delay, global)) {
                    files[i].delay = delay;
                    form.edited = true;
                }
            }
            if (!global) {
                ImGui::SameLine(0, 2 * scale);
                if (ImGui::SmallButton("×")) removed = static_cast<int>(i);
                help("Remove");
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    };
    tags(inherited, true);
    tags(files, false);
    if (inherited.empty() && files.empty()) mutedText(empty);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    if (removed >= 0) {
        files.erase(files.begin() + removed);
        form.edited = true;
    }
    ImGui::SameLine();
    if (iconButton(Icon::folder, choose))
        form.choose(window, add, false, files.empty() ? nullptr : filePath(files.back()).c_str());
    ImGui::PopID();
}
void dllField(Form &form, SDL_Window *window, float width, const std::vector<std::string> &inherited) {
    fileField(form, window, width, "DLLs",
        "Global DLLs load first, then account DLLs. Duplicate files load once. Changes apply next launch.", form.dlls,
        inherited, Form::dllPicker, "Add DLLs…", "Choose DLL files");
}
void programField(Form &form, SDL_Window *window, float width, const std::vector<Program> &inherited) {
    fileField(form, window, width, "Programs",
        "Started when you launch a game, unless already running, e.g. Blish HUD. Global programs start with "
        "every launch, then the account's own. Each waits its delay in seconds after the previous one. "
        "They keep running when the launcher closes and get the launcher's rights, the same as the game.",
        form.programs, inherited, Form::programPicker, "Add programs…", "Choose programs");
}
void qrCode(std::string_view bits, float width) {
    const int count = static_cast<int>(std::sqrt(bits.size()));
    if (!count || count * count != static_cast<int>(bits.size())) return;
    const float cell =
        std::max(1.f, std::floor(std::min(width, 180 * ImGui::GetStyle().FontScaleDpi) / count));
    const auto origin = ImGui::GetCursorScreenPos();
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + count * cell, origin.y + count * cell}, IM_COL32_WHITE);
    for (int i = 0; i < count * count; ++i) {
        if (bits[static_cast<std::size_t>(i)] != '1') continue;
        const ImVec2 pos{origin.x + (i % count) * cell, origin.y + (i / count) * cell};
        draw->AddRectFilled(pos, {pos.x + cell, pos.y + cell}, IM_COL32_BLACK);
    }
    ImGui::Dummy({count * cell, count * cell});
}
void signInChallenge(Form &form, const Snapshot::Authentication &auth, float width) {
    qrCode(auth.qr, width);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextWrapped("%s", auth.prompt.c_str());
    ImGui::PopTextWrapPos();
    if (auth.code) {
        rowField("Code", form.guard, width, "Steam Guard");
        if (iconButton(Icon::check, "Continue", true)) {
            form.request({.action = Action::guard, .id = form.id, .email = form.guard.data()});
            form.guard.fill(0);
        }
        ImGui::SameLine();
    }
    if (iconButton(Icon::close, "Cancel sign-in")) form.request({.action = Action::cancelConnect});
}
void platformFields(Form &form, const Snapshot::Authentication &auth, float width) {
    const bool matching = auth.id == form.id && auth.provider == form.provider;
    if (matching && !auth.identity.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextWrapped("%s", auth.username.c_str());
        mutedText(auth.identity.c_str());
        ImGui::PopTextWrapPos();
    }
    if (matching && auth.busy) {
        signInChallenge(form, auth, width);
        return;
    }
    if (form.provider == Provider::epic) {
        if (button(matching && !auth.identity.empty() ? "Reconnect Epic" : "Sign in to Epic", 0,
                ink().primary))
            form.request({.action = Action::connect, .id = form.id, .provider = Provider::epic});
        return;
    }
    if (form.steamPassword) {
        rowField("Steam", form.email, width, "Account name");
        rowField("Password", form.password, width, "Steam password",
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo);
    }
    if (button(
            matching && !auth.identity.empty() ? "Reconnect Steam" : "Sign in to Steam", 0, ink().primary)) {
        Command command{.action = Action::connect, .id = form.id, .provider = form.provider};
        if (form.steamPassword) {
            command.email = form.email.data();
            command.password = utf16(form.password.data());
        }
        form.request(std::move(command));
        form.clearPassword();
    }
    ImGui::SameLine();
    if (iconButton(form.steamPassword ? Icon::qr : Icon::password,
            form.steamPassword ? "Use QR code" : "Use password"))
        form.steamPassword = !form.steamPassword;
}
void registrationFields(Form &form, const std::string &id, float width) {
    if (form.displayNameId != id) {
        form.displayNameId = id;
        form.displayName.fill(0);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted("Choose your public name for this new GW2 account.");
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint(
        "##gw2-name", "3–27 letters and spaces", form.displayName.data(), form.displayName.size());
    ImGui::BeginDisabled(!validDisplayName(form.displayName.data()));
    if (iconButton(Icon::play, "Create account & play"))
        form.request({.action = Action::displayName, .id = id, .label = form.displayName.data()});
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (iconButton(Icon::close, "Cancel account setup"))
        form.request({.action = Action::cancelSetup, .id = id});
}
void verificationFields(Form &form, const SessionView &session, float width, bool focus) {
    const bool code = session.verification == 1 || session.verification == 5 || session.verification == 6;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    if ((session.verification == 4 || session.verification == 5) && !session.email.empty())
        ImGui::Text("Email: %s", session.email.c_str());
    ImGui::TextUnformatted(session.verification == 5 ? "Enter the five-digit code from your email."
        : session.verification == 1 ? "Enter the five-digit code from your text message."
        : session.verification == 6 ? "Enter the six-digit code from your authenticator."
        : session.verification == 4 ? "Approve this login using the link in your email."
                                   : "Approve this login in your authenticator app.");
    ImGui::PopTextWrapPos();
    if (!code) return;
    const auto digits = session.verification == 6 ? 6u : 5u;
    ImGui::BeginDisabled(session.state == 13);
    if (focus) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(width - 36 * ImGui::GetStyle().FontScaleDpi);
    const bool enter = ImGui::InputTextWithHint("##verification", "Verification code",
        form.verificationCode.data(), digits + 1,
        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_NoUndoRedo);
    ImGui::SameLine();
    const std::string_view value(form.verificationCode.data());
    const bool valid = value.size() == digits &&
        std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
    ImGui::BeginDisabled(!valid);
    const bool submit = iconButton(Icon::check, "Verify & continue", true);
    ImGui::EndDisabled();
    if (valid && session.state == 12 && (enter || submit)) {
        form.request({.action = Action::verify, .id = session.id,
            .password = utf16(value), .pid = session.pid});
        wipe(form.verificationCode.data(), form.verificationCode.size());
        clearInputMemory();
    }
    ImGui::EndDisabled();
}
void removeAccount(Form &form) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Remove?");
    ImGui::SameLine();
    if (iconButton(Icon::remove, "Confirm removal")) form.request({.action = Action::remove, .id = form.id});
    ImGui::SameLine();
    if (iconButton(Icon::close, "Cancel removal")) form.removing = false;
}
void accountFields(const Account *account, Form &form, const Snapshot &state, float width,
    SDL_Window *window) {
    form.edited |= rowField("Nickname", form.label, width, "Optional");
    // One row of buttons per line, sharing the width: the global platforms, then the China client's.
    const auto providerRow = [&](std::initializer_list<std::pair<Provider, const char *>> choices) {
        float labels{};
        for (const auto &choice : choices)
            labels += ImGui::CalcTextSize(choice.second).x;
        const auto count = static_cast<float>(choices.size());
        const auto padding = (width - labels - ImGui::GetStyle().ItemSpacing.x * (count - 1)) / count;
        bool first = true;
        for (const auto &[provider, name] : choices) {
            if (!std::exchange(first, false)) ImGui::SameLine();
            if (!button(name, ImGui::CalcTextSize(name).x + padding,
                    form.provider == provider ? ink().primary : ImVec4{}) ||
                form.provider == provider)
                continue;
            form.request({.action = Action::cancelConnect});
            // Anet and KongZhong logins are alike: switching between them keeps what was typed.
            if (!passwordLogin(form.provider) || !passwordLogin(provider)) {
                form.email.fill(0);
                form.clearPassword();
            }
            if (provider == Provider::wegame) form.proxy.enabled = false;
            form.provider = provider;
            form.edited = true;
        }
    };
    // Each installation offers its own accounts: Anet, Steam and Epic for the global client,
    // WeGame and KongZhong for the Chinese one; both, or none chosen yet, offer all. An
    // account keeps its own type's row.
    const auto &catalog = state.catalog;
    const bool anyGame = !catalog.gamePath.empty() || !catalog.chinaGamePath.empty();
    const bool globalRow = !anyGame || !catalog.gamePath.empty() || !chinaProvider(form.provider);
    const bool chinaRow = !anyGame || !catalog.chinaGamePath.empty() || chinaProvider(form.provider);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2{4 * ImGui::GetStyle().FontScaleDpi, ImGui::GetStyle().FramePadding.y});
    if (globalRow) providerRow({{Provider::arenaNet, "Anet"}, {Provider::steam, "Steam"}, {Provider::epic, "Epic"}});
    if (chinaRow) providerRow({{Provider::wegame, "WeGame"}, {Provider::kongZhong, "KongZhong"}});
    ImGui::PopStyleVar();
    const bool wegameAccount = form.provider == Provider::wegame;
    if (passwordLogin(form.provider)) {
        const bool kongZhong = form.provider == Provider::kongZhong;
        form.edited |= rowField(kongZhong ? "Account" : "Email", form.email, width,
            form.loading ? "Opening…" : kongZhong ? "KongZhong account" : "Anet email");
        form.edited |= rowField("Password", form.password, width, form.id.empty() ? "Password" : "Keep saved",
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo);
        if (kongZhong) help("For the China client. Signs in on its KongZhong login.");
    } else if (wegameAccount) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextWrapped("Signs in through WeGame. Keep WeGame open.");
        ImGui::PopTextWrapPos();
    } else {
        platformFields(form, state.auth, width);
    }
    // WeGame writes the game's command line and starts it, so neither applies there.
    if (!wegameAccount) {
        form.edited |= rowField("Arguments", form.args, width, "e.g. -windowed -loadmapinfo");
        help("Global arguments apply unless overridden here.");
        form.edited |= ImGui::Checkbox("Use a proxy for this account", &form.proxy.enabled);
        help("Each account can use a different proxy. Changes apply on its next launch. Requires mitmproxy.");
    }
    if (form.proxy.enabled && !wegameAccount) {
        ImGui::PushID("account-proxy");
        int protocol = form.proxy.protocol == "http" ? 1 : form.proxy.protocol == "https" ? 2 : 0;
        ImGui::SetNextItemWidth(width);
        if (ImGui::Combo("##proxy-type", &protocol, "SOCKS5\0HTTP CONNECT\0HTTPS CONNECT\0")) {
            form.proxy.protocol = protocol == 1 ? "http" : protocol == 2 ? "https" : "socks5";
            form.edited = true;
        }
        form.edited |= rowField("Host", form.proxyHost, width, "proxy.example.com");
        int port = static_cast<int>(form.proxy.port);
        const auto portOrigin = ImGui::GetCursorScreenPos();
        const auto portLabelWidth = std::max(68 * ImGui::GetStyle().FontScaleDpi,
            ImGui::CalcTextSize("Arguments").x + ImGui::GetStyle().ItemSpacing.x);
        ImGui::AlignTextToFramePadding();
        mutedText("Port");
        ImGui::SetCursorScreenPos({portOrigin.x + portLabelWidth, portOrigin.y});
        ImGui::SetNextItemWidth(width - portLabelWidth);
        if (ImGui::InputInt("##port", &port, 0, 0)) {
            form.proxy.port = static_cast<unsigned>(std::clamp(port, 0, 65536));
            form.edited = true;
        }
        help("Proxy port (1–65535).");
        form.edited |= rowField("Username", form.proxyUser, width, "Optional");
        const bool savedPassword = account && !account->proxy.protectedPassword.empty();
        if (rowField("Password", form.proxyPassword, width, savedPassword ? "Keep saved" : "Optional",
                ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo)) {
            form.edited = true;
            if (form.proxyPassword[0]) form.clearProxyPassword = false;
        }
        if (savedPassword && !form.proxyPassword[0])
            form.edited |= ImGui::Checkbox("Remove saved proxy password", &form.clearProxyPassword);
        if (form.proxyPassword[0])
            ImGui::TextWrapped("New password will be saved.");
        else if (form.clearProxyPassword)
            ImGui::TextWrapped("Saved password will be removed.");
        else if (savedPassword)
            ImGui::TextWrapped(form.proxyUser[0] ? "Password saved. Leave blank to keep it."
                                               : "Password saved. Enter a username to use it.");
        else if (form.proxyUser[0])
            ImGui::TextWrapped("No proxy password saved.");
        ImGui::TextWrapped("TCP uses this proxy without decrypting game traffic. SOCKS5 also supports UDP; HTTP proxies block UDP.");
#ifdef _WIN32
        ImGui::TextWrapped("Windows DNS and other applications use their existing connection.");
#else
        ImGui::TextWrapped("Wine and its child processes use an isolated connection, including DNS. The first launch copies your Wine prefix for this account. Requires mitmproxy, bubblewrap and iproute2.");
#endif
        ImGui::PopID();
    }
    dllField(form, window, width, state.catalog.dlls);
#ifdef _WIN32
    programField(form, window, width, state.catalog.programs);
#endif
    if (account && form.removing) {
        removeAccount(form);
        return;
    }
    if (!account) return;
    form.removing = iconButton(Icon::remove, "Remove account");
}

constexpr ImVec4 rowColor{20 / 255.f, 24 / 255.f, 29 / 255.f, 1};
struct AccountDrag {
    std::array<char, 37> id{};
    float grabY{};
};
struct AccountLayout {
    const SessionView *session;
    bool expanded;
    float reveal, height;
};
AccountLayout accountLayout(const Account *account, const Form &form, const Snapshot &state) {
    const auto session = account ? state.session(account->id) : nullptr;
    const bool expanded = (session && ((session->active && !session->closing && session->state == 9) ||
        session->needsVerification())) ||
        (form.page == Form::Page::account &&
            form.id == (account ? std::string_view(account->id) : std::string_view{}));
    ImGui::PushID(account ? account->id.c_str() : "new-account");
    const auto storage = ImGui::GetStateStorage();
    const float reveal = std::clamp(storage->GetFloat(ImGui::GetID("reveal")) +
        (expanded ? 1 : -1) * ImGui::GetIO().DeltaTime / 0.16f, 0.0f, 1.0f);
    const float height = 56 * ImGui::GetStyle().FontScaleDpi +
        storage->GetFloat(ImGui::GetID("height")) * reveal * reveal * (3 - 2 * reveal);
    ImGui::PopID();
    return {session, expanded, reveal, height};
}
// "Last played 3 h ago": the largest whole unit since `played` (Unix seconds), and in `date` its
// local date and time for the hover.
void playedText(std::int64_t played, std::span<char> text, std::span<char> date) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto ago = static_cast<long long>(std::max<std::int64_t>(now - played, 0));
    if (ago < 60)
        std::snprintf(text.data(), text.size(), "Last played %lld s ago", ago);
    else if (ago < 3600)
        std::snprintf(text.data(), text.size(), "Last played %lld min ago", ago / 60);
    else if (ago < 86400)
        std::snprintf(text.data(), text.size(), "Last played %lld h ago", ago / 3600);
    else
        std::snprintf(text.data(), text.size(), "Last played %lld d ago", ago / 86400);
    SDL_DateTime local{};
    if (SDL_TimeToDateTime(static_cast<SDL_Time>(played) * SDL_NS_PER_SECOND, &local, true))
        std::snprintf(date.data(), date.size(), "%04d-%02d-%02d %02d:%02d", local.year, local.month, local.day,
            local.hour, local.minute);
}
void accountRow(const Account *account, App &app, Form &form, const Snapshot &state,
    SDL_Window *nativeWindow, const AccountLayout &layout, bool floating = false) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto row = ImGui::GetCursorScreenPos();
    const auto width = ImGui::GetContentRegionAvail().x;
    const float header = 56 * scale, inset = 12 * scale;
    static const SessionView ready{{}, "Ready"};
    const auto &id = account ? account->id : ready.id;
    const auto [found, expanded, reveal, total] = layout;
    const auto &session = found ? *found : ready;
    const bool registration = account && session.active && !session.closing && session.state == 9;
    const bool verification = account && session.needsVerification();
    const bool busy = form.pending || form.picker || state.busy;
    ImGui::PushID(account ? account->id.c_str() : "new-account");
    auto storage = ImGui::GetStateStorage();
    const auto revealId = ImGui::GetID("reveal"), heightId = ImGui::GetID("height");
    storage->SetFloat(revealId, reveal);
    form.rowsMoving |= reveal != (expanded ? 1 : 0);
    if (!ImGui::IsRectVisible({width, total}) && !expanded) {
        ImGui::Dummy({width, total});
        ImGui::PopID();
        return;
    }
    const bool selected = account && form.isSelected(id);
    const auto idle = rowColor;
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(row, {row.x + width, row.y + total},
        ImGui::GetColorU32(selected ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : idle), 7 * scale);
    if (selected || floating)
        draw->AddRect(row, {row.x + width, row.y + total}, ImGui::GetColorU32(ink().accent), 7 * scale);
    const ImVec2 pos{row.x + inset, row.y + 8 * scale};
    const auto available = width - 2 * inset;
    const auto textWidth = available - 72 * scale;
    ImGui::SetCursorScreenPos(pos);
    const char *title = "Add account";
    if (account)
        title = !account->label.empty() ? account->label.c_str() : account->username.c_str();
    clippedText(title, textWidth);
    const auto statusY = pos.y + ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorScreenPos({pos.x, statusY});
    char pid[32]{};
    if (state.catalog.showPid && session.active && session.pid)
        std::snprintf(pid, sizeof(pid), " · %u", session.pid);
    const auto pidWidth = ImGui::CalcTextSize(pid).x;
    const bool queued = account && state.launchQueued(id);
    const bool thisLaunch = queued || session.blocking();
    const bool error = session.state == 6 ||
        (!session.active && session.status != "Ready" && session.status != "Client exited");
    // An account whose game is closed tells when it last closed, the date and time on hover.
    const auto played = account && !session.active && !queued && !error ? state.lastPlayed(id) : 0;
    std::array<char, 64> playedLine{};
    std::array<char, 32> playedDate{};
    if (played) playedText(played, playedLine, playedDate);
    const auto status = !account ? "Choose how you sign in"
        : queued                 ? "Queued"
        : error                  ? "Needs attention"
        : played                 ? playedLine.data()
                                 : session.status.c_str();
    const auto statusWidth = std::min(ImGui::CalcTextSize(status).x, textWidth - pidWidth);
    ImGui::PushStyleColor(ImGuiCol_Text, error ? ink().error : session.active ? ink().accent : ink().muted);
    clippedText(status, statusWidth);
    ImGui::PopStyleColor();
    if (error) help(session.status.c_str());
    if (played) help(playedDate.data());
    if (pidWidth) {
        ImGui::SetCursorScreenPos({pos.x + statusWidth, statusY});
        mutedText(pid);
    }
    const ImVec2 actions{pos.x + available - 64 * scale, row.y + (header - 28 * scale) / 2};
    ImGui::SetCursorScreenPos(actions);
    if (account) {
        ImGui::BeginDisabled(session.active ? !session.canClose()
                                            : thisLaunch || form.pending || form.picker || !state.ready ||
                state.updating());
        if (iconButton(session.active ? Icon::close : expanded ? Icon::collapse : Icon::edit,
                session.active ? "Kill game process"
                    : expanded ? "Close editor"
                               : "Edit account")) {
            if (session.active)
                form.request({.action = Action::close, .id = id, .pid = session.pid});
            else if (expanded)
                form.go(Form::Page::accounts);
            else
                form.go(Form::Page::account, id);
            form.rowsMoving = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool updating = state.updating();
        const char *launchTip = "Launch account";
        if (session.active) launchTip = "Show game";
        if (!session.active && queued) launchTip = "Queued";
        ImGui::BeginDisabled(session.active ? !session.canShow
                                            : updating || form.picker || !state.ready || queued || expanded);
        if (iconButton(session.active ? Icon::show : Icon::play, launchTip)) {
            form.request({.action = session.active ? Action::show : Action::launch, .id = id, .ids = {id}});
        }
        ImGui::EndDisabled();
        // Submit the row behind its buttons. Exclude even disabled buttons from selection.
        const auto mouse = ImGui::GetIO().MousePos;
        const bool onButton = mouse.y >= actions.y && mouse.y < actions.y + 28 * scale &&
            ((mouse.x >= actions.x && mouse.x < actions.x + 28 * scale) ||
                (mouse.x >= actions.x + 36 * scale && mouse.x < actions.x + 64 * scale));
        ImGui::SetCursorScreenPos(row);
        for (auto color : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive})
            ImGui::PushStyleColor(color, ImVec4{0, 0, 0, 0});
        ImGui::BeginDisabled(busy || onButton);
        if (ImGui::Selectable(
                "##select", selected, ImGuiSelectableFlags_NoPadWithHalfSpacing, {width, header})) {
            form.toggle(id);
            form.rowsMoving = true;
        }
        if (!busy && state.catalog.accounts.size() > 1 &&
            ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            AccountDrag drag;
            assign(drag.id, id);
            drag.grabY = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left].y - row.y;
            ImGui::SetDragDropPayload("GW2_ACCOUNT", &drag, sizeof(drag), ImGuiCond_Once);
            ImGui::EndDragDropSource();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
    } else {
        ImGui::SetCursorScreenPos(actions);
        ImGui::BeginDisabled(busy || reveal < 1 || !form.canSave(state));
        if (iconButton(Icon::check, "Add account", true)) form.save(app, state.catalog);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (iconButton(Icon::close, "Cancel new account")) form.go(Form::Page::accounts);
        ImGui::EndDisabled();
    }
    if (expanded) {
        auto window = ImGui::GetCurrentWindow();
        const auto measured = window->DC.CursorMaxPos;
        const bool fieldOnHighlight = selected && surface::active().style;
        if (fieldOnHighlight) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, idle);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, idle);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, idle);
        }
        ImGui::PushClipRect({row.x, row.y + header}, {row.x + width, row.y + total}, true);
        ImGui::SetCursorScreenPos({pos.x, row.y + header + 4 * scale});
        ImGui::BeginGroup();
        const bool lockEditor = form.pending || form.picker || !state.ready || state.updating() || thisLaunch;
        ImGui::BeginDisabled(reveal < 1 || (!registration && !verification && lockEditor));
        if (verification)
            verificationFields(form, session, available,
                form.focusVerification && form.verificationId == id && reveal == 1 && !form.pending && !form.picker);
        else if (registration)
            registrationFields(form, id, available);
        else
            accountFields(account, form, state, available, nativeWindow);
        ImGui::EndDisabled();
        ImGui::EndGroup();
        storage->SetFloat(heightId, ImGui::GetItemRectMax().y - (row.y + header) + 10 * scale);
        ImGui::PopClipRect();
        if (fieldOnHighlight) ImGui::PopStyleColor(3);
        // Measure the complete form, but publish only its revealed height to the list.
        // This keeps animation from creating a second scrolling region or a premature scroll extent.
        window->DC.CursorMaxPos = measured;
    }
    ImGui::SetCursorScreenPos(row);
    ImGui::Dummy({width, total});
    if (verification && form.focusVerification && form.verificationId == id) {
        ImGui::SetScrollHereY(0.5f);
        if (reveal == 1 && !form.pending && !form.picker) form.focusVerification = false;
    }
    ImGui::PopID();
}

void accountList(App &app, Form &form, const Snapshot &state, SDL_Window *nativeWindow, bool busy) {
    const auto first = ImGui::GetCursorScreenPos();
    const auto width = ImGui::GetContentRegionAvail().x;
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto mouse = ImGui::GetIO().MousePos;
    const auto &list = state.catalog.accounts;
    const Account *dragged{}, *before{};
    float dragY{};
    AccountLayout draggedLayout{};
    if (const auto payload = ImGui::GetDragDropPayload(); !busy && payload &&
        payload->IsDataType("GW2_ACCOUNT") && payload->DataSize == static_cast<int>(sizeof(AccountDrag))) {
        const auto &drag = *static_cast<const AccountDrag *>(payload->Data);
        const auto found = std::ranges::find(list, std::string_view(drag.id.data(), 36), &Account::id);
        if (found != list.end()) {
            dragged = &*found;
            dragY = mouse.y - drag.grabY;
            draggedLayout = accountLayout(dragged, form, state);
        }
    }
    for (const auto &account : list) {
        if (&account == dragged) continue;
        const auto layout = accountLayout(&account, form, state);
        // Choose the slot before inserting its gap so the preview cannot move its own threshold.
        if (dragged && !before && dragY < ImGui::GetCursorScreenPos().y + layout.height * 0.5f) {
            before = &account;
            ImGui::Dummy({width, draggedLayout.height});
        }
        accountRow(&account, app, form, state, nativeWindow, layout);
    }
    if (dragged) {
        if (!before) ImGui::Dummy({width, draggedLayout.height});
        const auto cursor = ImGui::GetCursorScreenPos();
        const auto extent = ImGui::GetCurrentWindow()->DC.CursorMaxPos;
        ImGui::SetCursorScreenPos({first.x, dragY});
        accountRow(dragged, app, form, state, nativeWindow, draggedLayout, true);
        ImGui::SetCursorScreenPos(cursor);
        ImGui::Dummy({0, 0});
        // The in-list gap owns scroll extent; the floating card must not grow it while scrolling.
        ImGui::GetCurrentWindow()->DC.CursorMaxPos = extent;
    }
    const auto clip = ImGui::GetCurrentWindow()->ClipRect;
    const ImRect target{{first.x, std::max(first.y - 2 * scale, clip.Min.y)}, {first.x + width, clip.Max.y}};
    if (!busy && list.size() > 1 && ImGui::BeginDragDropTargetCustom(target, ImGui::GetID("##reorder"))) {
        if (const auto payload = ImGui::AcceptDragDropPayload("GW2_ACCOUNT",
                ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            payload && dragged) {
            if (payload->IsDelivery())
                form.request({.action = Action::reorder, .id = dragged->id,
                    .beforeId = before ? before->id : std::string{}});
            else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const float edge = std::min(24 * scale, target.GetHeight() * 0.25f);
                const float direction = mouse.y < target.Min.y + edge ? -1.f
                    : mouse.y > target.Max.y - edge ? 1.f : 0.f;
                ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() + direction * 300 * scale *
                    ImGui::GetIO().DeltaTime, 0.f, ImGui::GetScrollMaxY()));
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void shortcuts(Form &form, bool busy, bool settingsLocked) {
    // Keys typed in the side panel or its dialogs belong to the panel.
    if (form.panelFocused) return;
    if (!busy && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N)) form.go(Form::Page::account);
    if (!settingsLocked && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma)) form.go(Form::Page::settings);
    if (form.page != Form::Page::accounts && ImGui::IsKeyPressed(ImGuiKey_Escape))
        form.go(Form::Page::accounts);
}
unsigned brandTexture = 0;
int brandTexels = 0;
struct Covered {
    float r{}, g{}, b{}, a{};
};
Covered coveredPixel(const unsigned char *pixels, int pitch, int width, int height, float x0, float y0,
    float x1, float y1) {
    Covered sum;
    float weight = 0;
    const int ix0 = static_cast<int>(x0);
    const int iy0 = static_cast<int>(y0);
    const int ix1 = std::min(width - 1, static_cast<int>(std::ceil(x1)) - 1);
    const int iy1 = std::min(height - 1, static_cast<int>(std::ceil(y1)) - 1);
    for (int iy = iy0; iy <= iy1; ++iy) {
        const float rowWeight = std::min(iy + 1.f, y1) - std::max(static_cast<float>(iy), y0);
        const auto *row = pixels + static_cast<std::size_t>(iy) * pitch;
        for (int ix = ix0; ix <= ix1; ++ix) {
            const float sampleWeight = rowWeight * (std::min(ix + 1.f, x1) - std::max(static_cast<float>(ix), x0));
            const auto *pixel = row + ix * 4;
            const float alpha = pixel[3] / 255.f;
            sum.r += pixel[0] * alpha * sampleWeight;
            sum.g += pixel[1] * alpha * sampleWeight;
            sum.b += pixel[2] * alpha * sampleWeight;
            sum.a += alpha * sampleWeight;
            weight += sampleWeight;
        }
    }
    if (weight <= 0) return sum;
    sum.r /= weight;
    sum.g /= weight;
    sum.b /= weight;
    sum.a /= weight;
    return sum;
}
void storeCovered(unsigned char *pixel, const Covered &sample) {
    const float alpha = std::clamp(sample.a, 0.f, 1.f);
    pixel[3] = static_cast<unsigned char>(std::lround(alpha * 255.f));
    if (alpha <= 0) return;
    pixel[0] = static_cast<unsigned char>(std::lround(std::clamp(sample.r / alpha, 0.f, 255.f)));
    pixel[1] = static_cast<unsigned char>(std::lround(std::clamp(sample.g / alpha, 0.f, 255.f)));
    pixel[2] = static_cast<unsigned char>(std::lround(std::clamp(sample.b / alpha, 0.f, 255.f)));
}
std::vector<unsigned char> fitIcon(const unsigned char *pixels, int pitch, int width, int height, int size) {
    std::vector<unsigned char> fitted(static_cast<std::size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        const float y0 = y * static_cast<float>(height) / size;
        const float y1 = (y + 1.f) * height / size;
        for (int x = 0; x < size; ++x) {
            const float x0 = x * static_cast<float>(width) / size;
            const float x1 = (x + 1.f) * width / size;
            storeCovered(fitted.data() + (static_cast<std::size_t>(y) * size + x) * 4,
                coveredPixel(pixels, pitch, width, height, x0, y0, x1, y1));
        }
    }
    return fitted;
}
unsigned loadBrandTexture(int texels) {
    const auto base = SDL_GetBasePath();
    if (!base) return 0;
    const auto file = std::string(base) + GW2_PROGRAM_FILE ".png";
    auto image = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
        SDL_LoadPNG(file.c_str()), SDL_DestroySurface);
    if (!image) return 0;
    auto converted = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
        SDL_ConvertSurface(image.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
    if (!converted || !converted->pixels || converted->w < 1 || converted->h < 1) return 0;
    const auto *pixels = static_cast<const unsigned char *>(converted->pixels);
    std::vector<unsigned char> fitted;
    int uploadW = converted->w, uploadH = converted->h;
    if (texels > 0 && texels < converted->w && texels < converted->h) {
        fitted = fitIcon(pixels, converted->pitch, converted->w, converted->h, texels);
        pixels = fitted.data();
        uploadW = uploadH = texels;
    }
    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, uploadW, uploadH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}
void ensureBrandTexture(SDL_Window *window) {
    int points = 0, pixels = 0;
    SDL_GetWindowSize(window, &points, nullptr);
    if (points <= 0) return;
    SDL_GetWindowSizeInPixels(window, &pixels, nullptr);
    const float frame = static_cast<float>(pixels) / static_cast<float>(points);
    const int texels =
        std::max(1, static_cast<int>(std::lround(24.f * ImGui::GetStyle().FontScaleDpi * frame)));
    if (brandTexture && texels == brandTexels) return;
    if (brandTexture) glDeleteTextures(1, &brandTexture);
    brandTexture = loadBrandTexture(texels);
    brandTexels = brandTexture ? texels : 0;
}
float sideInset(float scale) {
    if (!ImGui::GetCurrentContext()) return 8.f * scale;
    return ImGui::GetStyle().WindowPadding.x;
}
ImRect brandHitRect(float scale) {
    const float x = sideInset(scale);
    const float mark = 24 * scale;
    const float y = (headerHeight * scale - mark) / 2;
    if (brandTexture || !ImGui::GetCurrentContext()) return {{x, y}, {x + mark, y + mark}};
    const auto size = ImGui::CalcTextSize(surface::active().brand);
    const float textY = (headerHeight * scale - size.y) / 2;
    return {{x, textY}, {x + size.x, textY + size.y}};
}
void openHome(Form &form) {
    const auto *url = surface::active().home;
    if (!url || !*url) return;
    if (!SDL_OpenURL(url)) form.localError = "Could not open the link.";
}
void header(Form &form, SDL_Window *window, bool settingsLocked, bool updateAvailable) {
    ensureBrandTexture(window);
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto width = ImGui::GetWindowWidth();
    const float textY = (headerHeight * scale - ImGui::GetTextLineHeight()) / 2;
    const auto mark = brandHitRect(scale);
    ImGui::SetCursorPos(mark.Min);
    if (brandTexture)
        ImGui::Image(static_cast<ImTextureID>(brandTexture), mark.GetSize());
    else
        ImGui::TextUnformatted(surface::active().brand);
    ImGui::SetCursorPos(mark.Min);
    if (ImGui::InvisibleButton("##home", mark.GetSize())) openHome(form);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SameLine();
    ImGui::SetCursorPosY(textY);
    mutedText(surface::active().brandRest);
    const int first = headerButtons() - 3;
    if (first) {
        ImGui::SetCursorPos(headerButton(0, width, scale).Min);
        if (iconButton(Icon::files, surface::active().panel.label)) form.togglePanel = true;
        if (form.panelOpen) {
            const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect({min.x + scale, min.y + scale}, {max.x - scale, max.y - scale},
                ImGui::GetColorU32(ink().accent), ImGui::GetStyle().FrameRounding, 0, 1.25f * scale);
        }
    }
    ImGui::SetCursorPos(headerButton(first, width, scale).Min);
    ImGui::BeginDisabled(settingsLocked);
    if (iconButton(form.page != Form::Page::settings ? Icon::settings : Icon::back,
            form.page == Form::Page::settings ? "Back to accounts"
                : updateAvailable             ? "Settings · Update available"
                                              : "Settings")) {
        form.go(form.page != Form::Page::settings ? Form::Page::settings : Form::Page::accounts);
    }
    if (updateAvailable && form.page != Form::Page::settings) {
        const auto edge = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            {edge.x - 4 * scale, ImGui::GetItemRectMin().y + 4 * scale}, 3 * scale,
            ImGui::GetColorU32(ink().accent));
    }
    ImGui::EndDisabled();
    ImGui::SetCursorPos(headerButton(first + 1, width, scale).Min);
    if (iconButton(Icon::minimize, "Minimize")) SDL_MinimizeWindow(window);
    ImGui::SetCursorPos(headerButton(first + 2, width, scale).Min);
    if (iconButton(Icon::close, "Close launcher. Games keep running.")) form.go(Form::Page::exit);
    const auto inset = sideInset(scale);
    ImGui::SetCursorPos({inset, headerHeight * scale});
    ImGui::Separator();
    ImGui::SetCursorPos({inset, (headerHeight + 8) * scale});
}
void toolbar(Form &form, const Snapshot &state, bool busy) {
    if (form.page == Form::Page::settings) {
        heading("Settings");
        return;
    }
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto page = form.page;
    const bool closeAll = state.launchesQueued || std::ranges::any_of(state.sessions, &SessionView::canClose);
    const auto actionCount = 2 + closeAll;
    const auto width = ImGui::GetContentRegionAvail().x;
    const auto actionsWidth = (iconSize * actionCount + iconSpacing * (actionCount - 1)) * scale;
    const auto pos = ImGui::GetCursorScreenPos();
    const auto actionsX = ImGui::GetCursorPosX() + width - actionsWidth;
    ImGui::PushClipRect(pos, {pos.x + width - actionsWidth - 8 * scale, pos.y + 28 * scale}, true);
    heading("Accounts");
    ImGui::PopClipRect();
    ImGui::SameLine(actionsX);
    ImGui::BeginDisabled(busy);
    if (iconButton(Icon::add, "Add account")) form.go(Form::Page::account);
    ImGui::EndDisabled();
    ImGui::SameLine(0, iconSpacing * scale);
    const bool editingRequested = page == Form::Page::account && !form.id.empty() &&
        (form.selected.empty() || form.isSelected(form.id));
    ImGui::BeginDisabled(state.catalog.accounts.empty() || editingRequested || state.updating() ||
        !state.ready || form.picker);
    if (iconButton(Icon::playAll, form.selected.empty() ? "Launch all" : "Launch selected"))
        form.launch(state.catalog);
    ImGui::EndDisabled();
    if (closeAll) {
        ImGui::SameLine(0, iconSpacing * scale);
        if (iconButton(Icon::closeAll, "Kill all launched games and cancel queued launches"))
            form.request({.action = Action::closeAll});
    }
}
template <std::size_t N>
bool pathField(Form &form, Installation &draft, SDL_Window *window, const char *label, std::array<char, N> &value,
    unsigned target, bool folder, const char *hint) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    mutedText(label);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 36 * scale);
    const bool changed = ImGui::InputTextWithHint("##path", hint, value.data(), value.size());
    help(value.data());
    ImGui::SameLine();
    if (iconButton(Icon::folder, target == 1 || target == 4 ? "Browse for Gw2-64.exe" : "Browse"))
        form.choose(window, target, folder, value[0] ? value.data() : nullptr, &draft);
    ImGui::PopID();
    return changed;
}
bool pathCombo(const char *label, const char *value) {
    // Keep the menu above Settings; tooltips may extend beyond the native window.
    ImGui::SetNextWindowViewport(ImGui::GetWindowViewport()->ID);
    const bool open = ImGui::BeginCombo(label, value);
    if (!open) help(value);
    return open;
}
// Where GW2 is, shared by Settings, first-run setup, and launch repair: detection, the
// detected choices, the executable, and on Linux the Wine/Proton runner. Settings shows
// every field; the home card shows runner fields only while they are unusable. Returns
// whether the draft changed.
bool installationFields(Form &form, Installation &draft, SDL_Window *window, [[maybe_unused]] bool settings) {
    bool changed = draft.poll();
    ImGui::BeginDisabled(draft.detection.valid());
    if (button(draft.detection.valid() ? "Finding GW2…" : "Find again")) draft.detect();
    help("Look for Guild Wars 2 in its usual places. A valid choice is kept.");
    ImGui::EndDisabled();
    const auto *found = draft.found ? &*draft.found : nullptr;
    if (!draft.detectionError.empty())
        note(draft.detectionError);
    else if (found && !draft.detection.valid() && !draft.settled)
        note(found->games.empty() ? "No installation found. Browse for Gw2-64.exe."
                                  : "Several installations were found. Choose one.");
    if (found && !found->games.empty() &&
        pathCombo("Detected", draft.game[0] ? draft.game.data() : "Choose an installation")) {
        for (const auto &game : found->games) {
            const auto text = detectedGame(game);
            // Settings keeps the Chinese client in its own field; the home card has one.
            const bool chinese = settings && draft.isChina(text);
            auto &target = chinese ? draft.china : draft.game;
            if (ImGui::Selectable(chinese ? (text + "  (Chinese client)").c_str() : text.c_str(),
                    text == target.data())) {
                assign(target, text);
#ifndef _WIN32
                if (!chinese) assign(draft.prefix, game.prefix.string());
#endif
                changed = true;
            }
            help(text.c_str());
        }
        ImGui::EndCombo();
    }
    changed |= pathField(form, draft, window, "Game executable", draft.game, 1, false, "Full path to Gw2-64.exe");
    if (settings) {
        changed |= pathField(form, draft, window, "Chinese client", draft.china, 4, false,
            "Optional: KongZhong or WeGame Gw2-64.exe");
        help("For KongZhong and WeGame accounts.");
    }
#ifndef _WIN32
    const auto choosePath = [&](const char *label, const auto &values, auto &field) {
        if (values.empty() || !pathCombo(label, field.data())) return;
        for (const auto &value : values) {
            const auto text = std::filesystem::path(value).string();
            if (ImGui::Selectable(text.c_str(), text == field.data())) {
                assign(field, text);
                changed = true;
            }
            help(text.c_str());
        }
        ImGui::EndCombo();
    };
    if (settings || linux_setup::findExecutable(draft.runner.data(), linux_setup::environment("PATH")).empty()) {
        changed |= field("Runner", draft.runner, "wine, umu-run, or an absolute path");
        help(draft.runner.data());
        if (found) choosePath("Detected runners", found->runners, draft.runner);
    }
    if (settings || !linux_setup::prefix(draft.prefix.data())) {
        changed |= pathField(form, draft, window, "Wine prefix", draft.prefix, 2, true, "Directory containing system.reg");
        if (found) choosePath("Detected prefixes", found->prefixes, draft.prefix);
    }
    if (path(draft.runner.data()).filename() == "umu-run" && (settings || !linux_setup::proton(draft.proton.data()))) {
        changed |= pathField(form, draft, window, "Proton directory", draft.proton, 3, true,
            "Directory containing the proton executable");
        if (found) choosePath("Detected Proton", found->protons, draft.proton);
    }
#endif
    if (const auto problem = installationProblem(draft.fields()); !problem.empty()) note(problem);
    if (settings && draft.game[0] && draft.china[0] && !usableGame(draft.china.data()))
        note("The Chinese client's Gw2-64.exe is missing or incomplete.");
    return changed;
}
constexpr const char *normalLaunchHelp = "Sign in through GW2. Uses your remembered login if available.";
// The top of the home page: the normal launch and its session, first-run setup, and repair
// beside a launch that stopped at the installation. Repair keeps that request until it
// resumes once or is cancelled; detection alone never launches anything. With accounts,
// GW2 is one more row in their style that expands like an account editor.
void gameCard(App &app, Form &form, const Snapshot &state, SDL_Window *window, bool busy) {
    auto &draft = form.setup;
    const auto saved = installationProblem(state.catalog);
    const bool repair = form.recovery.open();
    const bool locked = form.pending || form.picker || !state.ready || state.updating();
    const bool expanded = repair || !saved.empty() || form.changing;
    const bool row = !state.catalog.accounts.empty();
    // Exactly one usable installation is saved for the player; several need a choice.
    if (draft.detected && !locked) {
        draft.detected = false;
        if (!saved.empty() && draft.settled && installationProblem(draft.fields()).empty())
            form.submit(app, {.action = Action::installation, .settings = draft.fields()});
    }
    if (std::exchange(form.scrollToSetup, false)) ImGui::SetScrollHereY(0);
    const auto *session = state.session(normalSession);
    const bool active = session && session->active;
    const bool queued = state.launchQueued(normalSession);
    const bool error = !queued && session && (session->state == 6 || (!active && session->status != "Client exited"));
    // The row can be hidden by choice, but never while it carries a session or a setup the player must see.
    if (row && state.catalog.hideGameRow && !active && !queued && !error && !expanded) return;
    const auto close = [&] {
        form.recovery.cancel();
        form.changing = false;
    };
    const auto show = [&] { form.request({.action = Action::show, .id = std::string(normalSession)}); };
    const auto kill = [&] {
        form.request({.action = Action::close, .id = std::string(normalSession), .pid = session->pid});
    };
    const auto launch = [&] { form.request({.action = Action::launchGame}); };
    const auto change = [&] {
        form.changing = true;
        draft.load(state.catalog, true);
        draft.detect();
    };
    // Buttons wrap instead of clipping in a narrow window.
    const auto beside = [](float right, const char *label) {
        const auto width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right) ImGui::SameLine();
    };
    // Choosing the installation, for first-run setup, Change, and repair.
    const auto setup = [&] {
        const auto right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        ImGui::BeginDisabled(locked || form.recovery.saving());
        installationFields(form, draft, window, false);
        ImGui::BeginDisabled(!installationProblem(draft.fields()).empty() || draft.same(state.catalog));
        if (button(repair ? "Use and launch" : "Use this installation", 0, ink().primary)) {
            form.submit(app, {.action = Action::installation, .settings = draft.fields()});
            form.recovery.save(form.pending);
        }
        ImGui::EndDisabled();
        if (repair && saved.empty()) {
            beside(right, "Continue launch");
            if (button("Continue launch", 0, ink().launch))
                form.resume(true, form.recovery.resume([&](const std::string &id) {
                    return std::ranges::find(state.catalog.accounts, id, &Account::id) != state.catalog.accounts.end();
                }));
        }
        if (repair || (form.changing && !row)) {
            beside(right, "Cancel");
            if (button("Cancel")) close();
        }
        ImGui::EndDisabled();
    };
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    if (row) {
        const auto origin = ImGui::GetCursorScreenPos();
        const auto width = ImGui::GetContentRegionAvail().x;
        const float header = 56 * scale, inset = 12 * scale, textWidth = width - 2 * inset - 72 * scale;
        auto *draw = ImGui::GetWindowDrawList();
        draw->ChannelsSplit(2); // The background goes behind contents measured afterwards.
        draw->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos({origin.x + inset, origin.y + 8 * scale});
        clippedText("Guild Wars 2", textWidth);
        ImGui::SetCursorScreenPos({origin.x + inset, origin.y + 8 * scale + ImGui::GetTextLineHeightWithSpacing()});
        const bool attention = error || (expanded && !form.changing);
        ImGui::PushStyleColor(ImGuiCol_Text, attention ? ink().error : active || queued ? ink().accent : ink().muted);
        clippedText(queued ? "Queued"
                : error    ? "Needs attention"
                : active   ? session->status.c_str()
                : repair   ? "Fix the game location to continue"
                : !saved.empty() ? "Choose your installation"
                : expanded ? "Choose another installation"
                           : "Sign in through GW2",
            textWidth);
        ImGui::PopStyleColor();
        help(error ? session->status.c_str() : active || expanded ? "" : normalLaunchHelp);
        ImGui::SetCursorScreenPos({origin.x + width - inset - 64 * scale, origin.y + (header - 28 * scale) / 2});
        ImGui::BeginDisabled(active ? !session->canClose() : locked || (expanded && !repair && !form.changing));
        if (iconButton(active ? Icon::close : expanded ? Icon::collapse : Icon::edit,
                active ? "Kill game process" : expanded ? "Close" : "Change game location"))
            active ? kill() : expanded ? close() : change();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(active ? !session->canShow : locked || queued || expanded);
        if (iconButton(active ? Icon::show : Icon::play, active ? "Show game" : "Launch GW2")) active ? show() : launch();
        ImGui::EndDisabled();
        auto bottom = origin.y + header;
        if (expanded) {
            ImGui::SetCursorScreenPos({origin.x + inset, bottom + 4 * scale});
            ImGui::BeginChild("##game-setup", {width - 2 * inset, 0},
                ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
            setup();
            ImGui::EndChild();
            bottom = ImGui::GetItemRectMax().y + 10 * scale;
        }
        draw->ChannelsSetCurrent(0);
        draw->AddRectFilled(origin, {origin.x + width, bottom}, ImGui::GetColorU32(rowColor), 7 * scale);
        draw->ChannelsMerge();
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy({width, bottom - origin.y});
        return;
    }
    // First run, with no accounts: a card that finds GW2, then offers a launch without one.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rowColor);
    ImGui::BeginChild("##game", {0, 0},
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleColor();
    const auto right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (expanded) {
        ImGui::TextWrapped("%s", repair ? "Fix the game location to continue launching."
                : saved.empty()         ? "Choose another installation."
                                        : "Where is Guild Wars 2?");
        setup();
    } else {
        ImGui::TextUnformatted("Guild Wars 2");
        ImGui::PushStyleColor(ImGuiCol_Text, ink().muted);
        clippedText(normalGamePath(state.catalog).c_str(), std::max(1.f, ImGui::GetContentRegionAvail().x -
            ImGui::CalcTextSize("Change").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginDisabled(locked);
        if (ImGui::SmallButton("Change")) change();
        ImGui::EndDisabled();
        if (queued || active || error)
            note(queued ? "Queued" : session->status, error ? ink().error : ink().accent);
        if (active) {
            ImGui::BeginDisabled(!session->canShow);
            if (button("Show game")) show();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!session->canClose());
            if (iconButton(Icon::close, "Kill game process")) kill();
            ImGui::EndDisabled();
        } else {
            ImGui::BeginDisabled(queued || locked);
            if (button("Launch GW2", 0, ink().launch)) launch();
            ImGui::EndDisabled();
        }
        beside(right, "Add account");
        ImGui::BeginDisabled(busy);
        if (button("Add account")) form.go(Form::Page::account);
        ImGui::EndDisabled();
        if (surface::shown(surface::Part::emptyState)) note(normalLaunchHelp);
    }
    ImGui::EndChild();
}
void settingsPage(Form &form, const Snapshot &state, SDL_Window *window) {
    form.edited |= installationFields(form, form.install, window, true);
    ImGui::Spacing();
    form.edited |= field("Global arguments", form.args, "e.g. -windowed -loadmapinfo");
#ifdef _WIN32
    form.edited |= field("mitmdump executable", form.mitmdump, "Optional full path to mitmdump.exe");
    help("Install mitmproxy from mitmproxy.org. Leave blank to find mitmdump.exe beside the launcher or on PATH. Proxy routing is enabled separately in each account.");
#else
    form.edited |= field("mitmdump executable", form.mitmdump, "Optional full path to Linux mitmdump");
    help("Install mitmproxy 12.2 or newer, bubblewrap and iproute2. Leave blank to find mitmdump on PATH. Proxy routing is enabled separately in each account.");
#endif
    dllField(form, window, ImGui::GetContentRegionAvail().x, {});
#ifdef _WIN32
    programField(form, window, ImGui::GetContentRegionAvail().x, {});
#endif
    form.edited |= ImGui::Checkbox("Hide sign-in window", &form.hide);
    help("Hide the GW2 sign-in window unless it needs your attention.");
    form.edited |= ImGui::Checkbox("Show PID", &form.showPid);
    form.edited |= ImGui::Checkbox("Always on top", &form.alwaysOnTop);
    help("Keep the launcher above other windows.");
    form.edited |= ImGui::Checkbox("Minimize when the game starts", &form.minimizeOnLaunch);
    help("Minimize the launcher once the games you launched are running.");
    form.edited |= ImGui::Checkbox("Hide Guild Wars 2 row", &form.hideGameRow);
    help("Hide the Guild Wars 2 row above your accounts. It returns while that game runs or needs attention.");
    ImGui::Spacing();
    ImGui::SeparatorText("Launcher updates");
    const auto &update = state.update;
    if (appUpdatesEnabled() && update.stage == UpdateStage::current)
        ImGui::TextDisabled("Version %s · Up to date", appVersion.data());
    else
        ImGui::TextDisabled("Version %s", appVersion.data());
    const auto updateNotice = appUpdateNotice();
    if (!updateNotice.empty()) ImGui::TextWrapped("%.*s", static_cast<int>(updateNotice.size()), updateNotice.data());
    if (!appUpdatesEnabled()) return;
    if (!appUpdatesAlwaysOn()) {
        form.edited |= ImGui::Checkbox("Check automatically", &form.autoUpdate);
        help("Check for launcher updates while the app is open.");
    }
    const bool available = update.stage == UpdateStage::available;
    ImGui::BeginDisabled(update.busy());
    const auto label = update.stage == UpdateStage::checking ? "Checking…"
        : update.stage == UpdateStage::installing            ? "Updating…"
        : available                                          ? "Update now"
                                                             : "Check for updates";
    if (button(label, 0, available ? ink().primary : ImVec4{}))
        form.request({.action = available ? Action::installUpdate : Action::checkUpdate});
    if (available) {
        ImGui::SameLine();
        if (button("Check again")) form.request({.action = Action::checkUpdate});
    }
    ImGui::EndDisabled();
    if (available) ImGui::TextDisabled("Version %s available", update.version.c_str());
    if (!update.error.empty()) ImGui::TextWrapped("%s", update.error.c_str());
}

void updatePrompt(Form &form, const Snapshot &state, bool busy) {
    const auto &update = state.update;
    if (update.stage == UpdateStage::available && state.catalog.autoUpdateAsked &&
        form.notifiedUpdate != update.version && state.ready && !form.picker && !form.pending &&
        !state.auth.busy && !ImGui::IsAnyItemActive()) {
        ImGui::OpenPopup("Update available");
        form.notifiedUpdate = update.version;
    }
    const auto viewport = ImGui::GetMainViewport();
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(300 * scale, viewport->Size.x - 28 * scale), 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, {0.5f, 0.5f});
    const bool open = ImGui::BeginPopupModal("Update available", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!open) return;
    char message[160];
    std::snprintf(message, sizeof message, "%s %s is available.", gw2ProductName(), update.version.c_str());
    centeredText(message);
    centerButtons({"Update now", "Later"});
    ImGui::BeginDisabled(busy || state.auth.busy);
    if (button("Update now", 0, ink().primary)) {
        form.openSettings(state.catalog);
        form.request({.action = Action::installUpdate});
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (button("Later")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void autoUpdatePrompt(Form &form, const Snapshot &state) {
    if (!state.catalog.autoUpdateAsked && !form.askedAutoUpdate && appUpdatesEnabled() && state.ready &&
        !form.picker && !form.pending && !state.auth.busy && !ImGui::IsAnyItemActive())
        ImGui::OpenPopup("Check automatically");
    const auto viewport = ImGui::GetMainViewport();
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(300 * scale, viewport->Size.x - 28 * scale), 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, {0.5f, 0.5f});
    const bool open = ImGui::BeginPopupModal("Check automatically", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!open) return;
    centeredText("Check for launcher updates automatically?");
    centerButtons({"Yes", "No"});
    const auto choose = [&](bool enable) {
        form.autoUpdate = enable;
        form.askedAutoUpdate = true;
        form.request({.action = Action::automaticUpdates, .enable = enable});
        ImGui::CloseCurrentPopup();
    };
    if (button("Yes", 0, ink().primary)) choose(true);
    ImGui::SameLine();
    if (button("No")) choose(false);
    ImGui::EndPopup();
}

// Width of the open side panel, including the splitter, out of a window this wide.
float panelPixels(const Form &form, float windowWidth, float scale) {
    const auto &panel = surface::active().panel;
    const auto least = panel.minWidth * scale;
    return std::round(std::clamp(form.panelWidth * scale, least,
        std::max(least, windowWidth - launcherMinWidth * scale)));
}
// Widen a normal window for the open panel, or give back exactly what was added, so the
// launcher column keeps its size. A maximized window waits until it is restored.
void fitPanel(Form &form, SDL_Window *window) {
    const auto &panel = surface::active().panel;
    const bool open = form.panelOpen && panel.draw;
    if (open == form.panelFitted ||
        (SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)))
        return;
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    int width{}, height{}, x{}, y{};
    SDL_GetWindowSize(window, &width, &height);
    const bool positioned = SDL_GetWindowPosition(window, &x, &y);
    const auto minHeight = static_cast<int>(160 * scale);
    if (!open) {
        SDL_SetWindowMinimumSize(window, static_cast<int>(launcherMinWidth * scale), minHeight);
        SDL_SetWindowSize(window, std::max(width - form.panelAdded, static_cast<int>(launcherMinWidth * scale)), height);
        // Put the window back where it was if opening the panel had to move it.
        if (form.panelShift && positioned && x == form.panelShift->second)
            SDL_SetWindowPosition(window, form.panelShift->first, y);
        form.panelShift.reset();
        form.panelAdded = 0;
        form.panelFitted = false;
        return;
    }
    // The panel gets the launcher's width, doubling the window, unless the screen is too narrow.
    form.panelWidth = std::max(panel.minWidth, static_cast<float>(width) / scale);
    auto target = width + static_cast<int>(std::round(form.panelWidth * scale));
    SDL_Rect bounds{};
    if (positioned && SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &bounds)) {
        target = std::max(width, std::min(target, bounds.w));
        if (x + target > bounds.x + bounds.w) {
            const auto moved = std::max(bounds.x, bounds.x + bounds.w - target);
            form.panelShift = std::pair{x, moved};
            SDL_SetWindowPosition(window, moved, y);
        }
    }
    SDL_SetWindowSize(window, target, height);
    form.panelAdded = target - width;
    form.panelWidth = std::max(panel.minWidth, static_cast<float>(form.panelAdded) / scale);
    form.panelFitted = true;
    SDL_SetWindowMinimumSize(window, static_cast<int>((launcherMinWidth + panel.minWidth) * scale), minHeight);
}
void togglePanel(Form &form, SDL_Window *window) {
    form.togglePanel = false;
    if (!surface::active().panel.draw) return;
    form.panelOpen = !form.panelOpen;
    // A maximized window splits in half until it is restored.
    if (form.panelOpen && (SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        int width{}, height{};
        SDL_GetWindowSize(window, &width, &height);
        form.panelWidth = std::max(surface::active().panel.minWidth,
            static_cast<float>(width) / 2 / ImGui::GetStyle().FontScaleDpi);
    }
    fitPanel(form, window);
}
// While the side panel is open, the launcher draws in a left column of its own width.
void beginLauncherColumn(const Form &form) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto panel = panelPixels(form, ImGui::GetWindowWidth(), scale);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{});
    ImGui::BeginChild("##launcher", {std::max(1.f, ImGui::GetContentRegionAvail().x - panel), 0},
        ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
}
void panelColumn(Form &form) {
    ImGui::EndChild();
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto &panel = surface::active().panel;
    // The divider keeps the window's edge padding on both sides.
    const auto gap = 2 * ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine(0, 0);
    // Dragging the splitter trades width between the two columns; the window keeps its size.
    ImGui::InvisibleButton("##panel-splitter", {gap, std::max(1.f, ImGui::GetContentRegionAvail().y)});
    const bool dragging = ImGui::IsItemActive();
    if (dragging || ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (dragging) {
        const auto right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        const auto most = std::max(panel.minWidth, (ImGui::GetWindowWidth() - launcherMinWidth * scale) / scale);
        form.panelWidth = std::clamp((right - ImGui::GetIO().MousePos.x + gap * 0.5f) / scale, panel.minWidth, most);
    }
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const auto line = std::floor((min.x + max.x) * 0.5f);
    ImGui::GetWindowDrawList()->AddLine({line, min.y + 4 * scale}, {line, max.y - 4 * scale},
        ImGui::GetColorU32(dragging || ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator),
        scale);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{});
    ImGui::BeginChild("##panel", {0, 0}, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    panel.draw();
    form.panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::EndChild();
}

void render(App &app, Form &form, const Snapshot &state, SDL_Window *window, bool &open) {
    form.observe(state);
    form.observePicker();
    // Look for the game once at startup when the saved installation cannot launch.
    if (state.ready && !std::exchange(form.setupChecked, true) && !installationProblem(state.catalog).empty()) {
        form.setup.load(state.catalog, true);
        form.setup.detect();
    }
    form.rowsMoving = false;
    const bool busy = form.pending || form.picker || state.busy || !state.ready;
    const bool settingsLocked = form.pending || form.picker || !state.ready || state.updating();
    auto vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::Begin(gw2ProductName(), nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse);
    surface::paint(surface::Insert::beforeHeader);
    if (surface::shown(surface::Part::header))
        header(form, window, settingsLocked, state.update.stage == UpdateStage::available);
    surface::paint(surface::Insert::afterHeader);
    shortcuts(form, busy, settingsLocked);
    const auto &message = form.localError.empty() ? state.error.message : form.localError;
    const auto page = form.page;
    const bool accounts = page != Form::Page::settings;
    const bool panel = form.panelOpen && surface::active().panel.draw;
    if (panel) beginLauncherColumn(form);
    else form.panelFocused = false;
    surface::paint(surface::Insert::beforeToolbar);
    if (surface::shown(surface::Part::toolbar)) toolbar(form, state, busy);
    surface::paint(surface::Insert::afterToolbar);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * form.opacity());
    // Each page owns its scroll position; navigation never reuses another form's scroll offset.
    ImGui::PushID(accounts ? 0 : 1);
    if (form.scrollToNew) {
        ImGui::SetNextWindowScroll({0, 0});
        form.scrollToNew = false;
    }
    ImGui::BeginChild("page");
    surface::paint(surface::Insert::beforePage);
    if (!message.empty() && surface::shown(surface::Part::error)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ink().error);
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    if (!state.ready) {
        if (surface::shown(surface::Part::loading))
            mutedText(state.fatal ? "Your saved accounts have not been changed." : "Opening accounts…");
    } else if (accounts) {
        if (const auto update = state.session(""); update && update->active) {
            mutedText(update->status.c_str());
            ImGui::Spacing();
        }
        gameCard(app, form, state, window, busy);
        if (surface::shown(surface::Part::accounts) && form.page == Form::Page::account && form.id.empty())
            accountRow(nullptr, app, form, state, window, accountLayout(nullptr, form, state));
        if (surface::shown(surface::Part::accounts)) accountList(app, form, state, window, busy);
    } else {
        ImGui::BeginDisabled(settingsLocked || form.page != page);
        if (surface::shown(surface::Part::settings)) settingsPage(form, state, window);
        ImGui::EndDisabled();
        surface::paint(surface::Insert::settingsTail);
    }
    surface::paint(surface::Insert::afterPage);
    ImGui::EndChild();
    ImGui::PopID();
    ImGui::PopStyleVar();
    if (panel) panelColumn(form);
    if (surface::shown(surface::Part::updatePrompt)) {
        autoUpdatePrompt(form, state);
        updatePrompt(form, state, busy);
    }
    // Consume this frame's input before saving or acting on a navigation request.
    form.finish(app, state, open);
    ImGui::End();
}
void drawFrame(
    App &app, Form &form, const Snapshot &snapshot, SDL_Window *window, SDL_GLContext context, bool &open) {
    auto &io = ImGui::GetIO();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuiErrorRecoveryState stacks;
    ImGui::ErrorRecoveryStoreState(&stacks);
    try {
        render(app, form, snapshot, window, open);
    } catch (const std::exception &e) {
        form.localError = e.what();
        const auto previous = io.ConfigErrorRecoveryEnableAssert;
        io.ConfigErrorRecoveryEnableAssert = false;
        ImGui::ErrorRecoveryTryToRecoverState(&stacks);
        io.ConfigErrorRecoveryEnableAssert = previous;
    }
    ImGui::Render();
    SDL_GL_MakeCurrent(window, context);
    int width{}, height{};
    SDL_GetWindowSizeInPixels(window, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(ink().background.x, ink().background.y, ink().background.z, ink().background.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SwapWindow(window);
}
struct ResizeWatch {
    App &app;
    Form &form;
    SDL_Window *window;
    SDL_GLContext context;
    bool &open;
    bool drawing{};
    std::exception_ptr error{};
    static bool SDLCALL event(void *data, SDL_Event *event) noexcept {
        if (event->type != SDL_EVENT_WINDOW_EXPOSED || event->window.data1 != 1 || !SDL_IsMainThread())
            return true;
        auto &self = *static_cast<ResizeWatch *>(data);
        if (!self.open || self.drawing || event->window.windowID != SDL_GetWindowID(self.window)) return true;
        self.drawing = true;
        try {
            drawFrame(self.app, self.form, *self.app.snapshot(), self.window, self.context, self.open);
        } catch (...) {
            self.error = std::current_exception();
            self.open = false;
        }
        self.drawing = false;
        return true;
    }
    ~ResizeWatch() { SDL_RemoveEventWatch(event, this); }
};
bool pollEvents(App &app, Form &form, SDL_Window *window, SDL_GLContext context) {
    const auto &io = ImGui::GetIO();
    bool open = true;
    // Windows owns the message loop during a border drag. Repaint on SDL's
    // main-thread expose callback, only while our ordinary frame is inactive.
    ResizeWatch resize{app, form, window, context, open};
    if (!SDL_AddEventWatch(ResizeWatch::event, &resize)) throw std::runtime_error(SDL_GetError());
    SDL_Event event{};
    const auto &gui = *ImGui::GetCurrentContext();
    const bool tooltipPending =
        gui.HoverItemDelayId && gui.HoverItemDelayTimer < ImGui::GetStyle().HoverDelayShort;
    const bool interactive = !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) &&
        (io.WantTextInput || io.MouseDown[0] || form.animating() || tooltipPending);
    if (SDL_WaitEventTimeout(&event, interactive ? 8 : 125))
        do {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data && form.panelOpen &&
                event.drop.windowID == SDL_GetWindowID(window))
                if (const auto drop = surface::active().panel.dropFile) drop(event.drop.data);
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                    event.window.windowID == SDL_GetWindowID(window)))
                open = false;
        } while (SDL_PollEvent(&event));
    if (resize.error) std::rethrow_exception(resize.error);
    return open;
}
void restoreWindow(SDL_Window *window, const WindowState &saved) {
    const SDL_Point point{saved.x, saved.y};
    const auto display = saved.positioned ? SDL_GetDisplayForPoint(&point) : SDL_GetPrimaryDisplay();
    const float scale = std::max(1.0f, saved.scale ? saved.scale : SDL_GetDisplayContentScale(display));
    // Restore one UI scale with the saved dimensions. Monitor changes must not
    // rescale the layout or make the header's native hit regions drift from it.
    style(scale);
    int width = static_cast<int>(saved.width * scale), height = static_cast<int>(saved.height * scale);
    SDL_Rect bounds{};
    const bool bounded = SDL_GetDisplayUsableBounds(display, &bounds);
    if (bounded) {
        width = std::min(width, bounds.w);
        height = std::min(height, bounds.h);
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(240 * scale), static_cast<int>(160 * scale));
    SDL_SetWindowSize(window, width, height);
    if (bounded)
        SDL_SetWindowPosition(window,
            saved.positioned ? std::clamp(saved.x, bounds.x, bounds.x + bounds.w - width)
                             : bounds.x + (bounds.w - width) / 2,
            saved.positioned ? std::clamp(saved.y, bounds.y, bounds.y + bounds.h - height)
                             : bounds.y + (bounds.h - height) / 2);
}
auto createWindow() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    const auto display = SDL_GetPrimaryDisplay();
    const float scale = std::max(1.0f, SDL_GetDisplayContentScale(display));
    auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(
        SDL_CreateWindow(gw2ProductName(), static_cast<int>(360 * scale), static_cast<int>(480 * scale),
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow);
    if (!window) throw std::runtime_error(SDL_GetError());
#ifndef _WIN32
    // Windows uses SDL's embedded ICO resource so the shell can select the matching size.
    const auto base = SDL_GetBasePath();
    if (!base) throw std::runtime_error(SDL_GetError());
    const auto iconFile = std::string(base) + GW2_PROGRAM_FILE ".png";
    auto icon = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
        SDL_LoadPNG(iconFile.c_str()), SDL_DestroySurface);
    if (!icon || !SDL_SetWindowIcon(window.get(), icon.get())) throw std::runtime_error(SDL_GetError());
#endif
    SDL_SetWindowPosition(window.get(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_SetWindowHitTest(window.get(), hitTest, nullptr);
    return window;
}
// The launcher speaks Chinese when the only Guild Wars 2 here is the Chinese client: saved, or
// found by a detection. A global installation, with or without the Chinese one, keeps English.
bool chineseOnly(const Catalog &catalog, const Form &form) {
    bool global = !catalog.gamePath.empty(), china = !catalog.chinaGamePath.empty();
    for (const auto *install : {&form.install, &form.setup}) {
        if (!install->found) continue;
        for (const auto &entry : install->found->games)
            (install->isChina(detectedGame(entry)) ? china : global) = true;
    }
    return china && !global;
}
void setupFonts(float scale) {
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable |
        ImGuiConfigFlags_ViewportsEnable;
    ImFontConfig font;
    font.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char *>(launcherFont), sizeof(launcherFont), 16, &font);
    style(scale);
}
} // namespace gw2::presentation

export namespace gw2 {
int run() {
    using namespace presentation;
    const auto instance = acquireInstance();
    if (!instance) return 0;
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "none");
    SDL_SetHint(SDL_HINT_APP_NAME, gw2ProductName());
    if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    struct Sdl {
        ~Sdl() { SDL_Quit(); }
    } sdl;
    auto window = createWindow();
    const float scale = std::max(1.0f, SDL_GetWindowDisplayScale(window.get()));
    auto context = SDL_GL_CreateContext(window.get());
    if (!context) throw std::runtime_error(SDL_GetError());
    struct Gl {
        SDL_GLContext context;
        ~Gl() { SDL_GL_DestroyContext(context); }
    } gl{context};
    SDL_GL_SetSwapInterval(1);
    ImGui::SetAllocatorFunctions(allocate, release);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    struct Gui {
        bool platform{}, renderer{};
        ~Gui() {
            if (renderer) ImGui_ImplOpenGL3_Shutdown();
            if (platform) ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }
    } gui;
    surface::install();
    struct SurfaceLifetime {
        ~SurfaceLifetime() {
            if (const auto stop = surface::active().shutdown) stop();
        }
    } surfaceLifetime;
    setupFonts(scale);
    text::install();
    if (!(gui.platform = ImGui_ImplSDL3_InitForOpenGL(window.get(), context)) ||
        !(gui.renderer = ImGui_ImplOpenGL3_Init("#version 130")))
        throw std::runtime_error("Could not initialize the launcher renderer.");
    struct Brand {
        ~Brand() {
            if (brandTexture) glDeleteTextures(1, &brandTexture);
            brandTexture = 0;
        }
    } brand;
    App app;
    Form form;
    bool open = true;
    bool restored{};
    WindowState observed;
    std::shared_ptr<const Snapshot> previous;
    while (open) {
        if (const auto poll = surface::active().poll) poll();
        if (!pollEvents(app, form, window.get(), context)) form.go(Form::Page::exit);
        const auto snapshot = app.snapshot();
        if (snapshot != previous) {
            if (snapshot->ready && (!previous || !previous->ready ||
                    snapshot->catalog.alwaysOnTop != previous->catalog.alwaysOnTop)) {
                if (!SDL_SetWindowAlwaysOnTop(window.get(), snapshot->catalog.alwaysOnTop))
                    form.localError = "Could not apply Always on top: " + std::string(SDL_GetError());
            }
            // A launch is done once a game it started is running and nothing else is pending.
            // Games recalled at startup were already running, so they never minimize.
            if (snapshot->catalog.minimizeOnLaunch && previous && previous->ready && !snapshot->busy &&
                std::ranges::any_of(snapshot->sessions, [&](const SessionView &session) {
                    const auto old = previous->session(session.id);
                    return session.active && session.state == 4 && (!old || !old->active || old->state != 4);
                }))
                SDL_MinimizeWindow(window.get());
            for (const auto &session : snapshot->sessions) {
                if (!session.active || session.state != 12) continue;
                const auto old = previous ? previous->session(session.id) : nullptr;
                if (old && old->active && old->pid == session.pid && old->state == 12) continue;
                wipe(form.verificationCode.data(), form.verificationCode.size());
                form.verificationId = session.id;
                form.verificationPid = session.pid;
                form.focusVerification = true;
                if (form.page == Form::Page::settings) form.go(Form::Page::accounts);
                if (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(window.get());
                SDL_RaiseWindow(window.get());
                SDL_FlashWindow(window.get(), SDL_FLASH_UNTIL_FOCUSED);
                break;
            }
            previous = snapshot;
        }
        if (!restored && (snapshot->ready || snapshot->fatal)) {
            observed = snapshot->catalog.window;
            restoreWindow(window.get(), observed);
            SDL_ShowWindow(window.get());
            restored = true;
        }
        // Applies a panel opened or closed while maximized once the window is restored.
        if (restored) fitPanel(form, window.get());
        const auto flags = SDL_GetWindowFlags(window.get());
        if (restored && snapshot->ready && !(flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED))) {
            WindowState current;
            int width{}, height{};
            SDL_GetWindowSize(window.get(), &width, &height);
            current.scale = ImGui::GetStyle().FontScaleDpi;
            // Remember the launcher's own width; the side panel opens closed next time.
            const auto launcherWidth = static_cast<float>(width - (form.panelFitted ? form.panelAdded : 0));
            current.width = static_cast<int>(std::lround(launcherWidth / current.scale));
            current.height = static_cast<int>(std::lround(height / current.scale));
            current.positioned = SDL_GetWindowPosition(window.get(), &current.x, &current.y);
            if (form.panelFitted && form.panelShift && current.x == form.panelShift->second)
                current.x = form.panelShift->first;
            if (current != observed) {
                app.rememberWindow(current);
                observed = current;
            }
        }
        if (!restored || (flags & SDL_WINDOW_MINIMIZED)) {
            form.observe(*snapshot);
            form.finish(app, *snapshot, open);
            continue;
        }
        if (form.togglePanel) togglePanel(form, window.get());
        text::setChinese(chineseOnly(snapshot->catalog, form));
        drawFrame(app, form, *snapshot, window.get(), context, open);
    }
    form.close();
    if (app.snapshot()->update.stage == UpdateStage::installed) return 3;
    return surface::active().restart ? 4 : 0;
}
} // namespace gw2

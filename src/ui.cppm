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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include "font.hpp"

export module ui;
import platform;
import accounts;
import app;

namespace gw2::presentation {
constexpr ImVec4 background{0.055f, 0.067f, 0.082f, 1};
constexpr ImVec4 accent{0.68f, 0.55f, 1, 1};
constexpr ImVec4 primaryColor{0.44f, 0.22f, 0.86f, 1};
constexpr ImVec4 launchColor{0.09f, 0.45f, 0.25f, 1};
constexpr ImVec4 closeColor{0.72f, 0.17f, 0.28f, 1};
constexpr ImVec4 muted{0.54f, 0.59f, 0.65f, 1};
constexpr ImVec4 errorColor{1, 0.55f, 0.48f, 1};
constexpr float iconSize = 28, iconSpacing = 4, headerHeight = 42;
ImRect headerButton(int index, float width, float scale) {
    const ImVec2 pos{
        width - (14 + 3 * iconSize + 2 * iconSpacing) * scale + index * (iconSize + iconSpacing) * scale,
        (headerHeight - iconSize) * scale / 2};
    return {pos, {pos.x + iconSize * scale, pos.y + iconSize * scale}};
}
template <std::size_t N> void assign(std::array<char, N> &out, std::string_view text) {
    const auto size = std::min(N - 1, text.size());
    std::memcpy(out.data(), text.data(), size);
    out[size] = 0;
}
void mutedText(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}
void help(const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_NoSharedDelay |
            ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
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
    closeAll,
    edit,
    show,
    settings,
    back,
    minimize,
    close,
    check,
    remove,
    folder,
    password,
    qr
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
    }
}
bool iconButton(Icon icon, const char *label, bool primary = false) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    auto color = primary ? primaryColor : ImVec4{};
    if (icon == Icon::play) color = launchColor;
    if (icon == Icon::close || icon == Icon::closeAll || icon == Icon::remove) color = closeColor;
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
    std::string value, error;
};
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
    std::string id, localError;
    std::array<char, 321> label{};
    std::array<char, 1281> email{};
    std::array<char, 1025> password{};
    std::array<char, 6> guard{};
    std::array<char, 28> displayName{};
    std::string displayNameId;
    Provider provider{};
    bool steamPassword{};
    App *authApp{};
    std::array<char, 2049> args{};
    std::array<char, 32761> game{}, runner{}, prefix{}, proton{};
    bool hide{}, showPid{}, removing{}, loading{}, edited{}, connected{};
    std::uint64_t pending{};
    Action pendingAction{};
    std::shared_ptr<Picker> picker;
    unsigned pickerField{};
    ~Form() { wipe(password.data(), password.size()); }
    void navigate(Page target) {
        if (page == target) return;
        if ((page == Page::settings) != (target == Page::settings)) changedAt = ImGui::GetTime();
        page = target;
    }
    bool transitioning() const { return rowsMoving || ImGui::GetTime() - changedAt < 0.16; }
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
        wipe(password.data(), password.size());
        clearInputMemory();
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
        assign(game, c.gamePath);
        assign(args, c.arguments);
        assign(runner, c.runner);
        assign(prefix, c.prefix);
        assign(proton, c.proton);
        hide = c.hideLogin;
        showPid = c.showPid;
    }
    void openAccount(const Account *account, App &app) {
        close();
        authApp = &app;
        provider = account ? account->provider : Provider::arenaNet;
        steamPassword = false;
        guard.fill(0);
        navigate(Page::account);
        scrollToNew = !account;
        assign(label, account ? account->label : "");
        assign(email, "");
        assign(args, account ? account->arguments : "");
        if (account) {
            id = account->id;
            loading = true;
            submit(app, {.action = Action::edit, .id = id});
        }
    }
    void observe(const Snapshot &snapshot) {
        const bool signedIn =
            snapshot.auth.connected && snapshot.auth.id == id && snapshot.auth.provider == provider;
        if (page == Page::account && !id.empty() && signedIn && !connected) edited = true;
        connected = signedIn;
        if (!displayNameId.empty()) {
            const auto session = snapshot.session(displayNameId);
            if (!session || !session->active || session->state == 4) {
                displayName.fill(0);
                displayNameId.clear();
            }
        }
        std::erase_if(selected, [&](const auto &id) {
            return std::ranges::find(snapshot.catalog.accounts, id, &Account::id) ==
                snapshot.catalog.accounts.end();
        });
        if (!pending || snapshot.completed < pending) return;
        pending = 0;
        loading = false;
        if (!snapshot.error.empty()) {
            next = std::monostate{};
            if (pendingAction == Action::save || pendingAction == Action::settings)
                localError = "Changes were not saved. " + snapshot.error;
            return;
        }
        if (pendingAction == Action::edit && snapshot.editId == id) assign(email, snapshot.editEmail);
        if (pendingAction == Action::save) {
            wipe(password.data(), password.size());
            clearInputMemory();
        }
        if ((pendingAction == Action::save && id.empty()) || pendingAction == Action::remove) close();
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
            c.settings.gamePath = game.data();
            c.settings.arguments = args.data();
            c.settings.hideLogin = hide;
            c.settings.showPid = showPid;
            c.settings.runner = runner.data();
            c.settings.prefix = prefix.data();
            c.settings.proton = proton.data();
        } else {
            c.action = Action::save;
            c.id = id;
            c.label = label.data();
            c.email = email.data();
            c.arguments = args.data();
            c.provider = provider;
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
        if (provider == Provider::arenaNet) return email[0] && password[0];
        return state.auth.connected && state.auth.id == id && state.auth.provider == provider;
    }
    void finish(App &app, const Snapshot &state, bool &open) {
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
            openAccount(destination->id.empty() ? nullptr : &*account, app);
            return;
        }
        close();
        if (destination->page == Page::exit) open = false;
    }
    void choose(SDL_Window *window, unsigned target, bool folder, const char *initial) {
        picker = std::make_shared<Picker>();
        pickerField = target;
        auto data = new std::shared_ptr<Picker>(picker);
        auto callback = +[](void *opaque, const char *const *files, int) {
            std::unique_ptr<std::shared_ptr<Picker>> owner(static_cast<std::shared_ptr<Picker> *>(opaque));
            auto &reply = **owner;
            std::lock_guard lock(reply.mutex);
            if (!files)
                reply.error = "The file picker could not open. Enter the path directly.";
            else if (files[0])
                reply.value = files[0];
            reply.done = true;
        };
        if (folder)
            SDL_ShowOpenFolderDialog(callback, data, window, initial, false);
        else
            SDL_ShowOpenFileDialog(callback, data, window, nullptr, 0, initial, false);
    }
    void observePicker() {
        if (!picker) return;
        {
            std::lock_guard lock(picker->mutex);
            if (!picker->done) return;
            localError = picker->error;
            if (!picker->value.empty()) {
                assign(pickerField == 1 ? game : pickerField == 2 ? prefix : proton, picker->value);
                edited = true;
            }
        }
        picker.reset();
    }
};

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
    for (int i = 0; i < 3; ++i)
        if (headerButton(i, static_cast<float>(w), scale).Contains(position)) return SDL_HITTEST_NORMAL;
    return SDL_HITTEST_DRAGGABLE;
}
void style(float scale) {
    auto &s = ImGui::GetStyle();
    s = ImGuiStyle{};
    ImGui::StyleColorsDark();
    s.WindowPadding = {14, 10};
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
    s.Colors[ImGuiCol_WindowBg] = background;
    s.Colors[ImGuiCol_PopupBg] = {0.078f, 0.094f, 0.114f, 1};
    s.Colors[ImGuiCol_Border] = {0.16f, 0.19f, 0.22f, 1};
    s.Colors[ImGuiCol_FrameBg] = {0.095f, 0.114f, 0.137f, 1};
    s.Colors[ImGuiCol_FrameBgHovered] = {0.13f, 0.16f, 0.19f, 1};
    s.Colors[ImGuiCol_FrameBgActive] = {0.14f, 0.18f, 0.21f, 1};
    s.Colors[ImGuiCol_Text] = {0.90f, 0.93f, 0.95f, 1};
    s.Colors[ImGuiCol_TextDisabled] = muted;
    s.Colors[ImGuiCol_Button] = {0.12f, 0.15f, 0.18f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.19f, 0.24f, 0.28f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {0.23f, 0.29f, 0.32f, 1};
    s.Colors[ImGuiCol_Header] = {0.16f, 0.12f, 0.22f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {0.21f, 0.16f, 0.29f, 1};
    s.Colors[ImGuiCol_HeaderActive] = primaryColor;
    s.Colors[ImGuiCol_CheckMark] = accent;
    s.Colors[ImGuiCol_CheckboxSelectedBg] = s.Colors[ImGuiCol_FrameBgHovered];
    s.Colors[ImGuiCol_Separator] = s.Colors[ImGuiCol_Border];
    s.Colors[ImGuiCol_SeparatorHovered] = s.Colors[ImGuiCol_SeparatorActive] = accent;
    s.Colors[ImGuiCol_ResizeGrip] = s.Colors[ImGuiCol_Border];
    s.Colors[ImGuiCol_ResizeGripHovered] = s.Colors[ImGuiCol_ResizeGripActive] = accent;
    s.Colors[ImGuiCol_SliderGrab] = primaryColor;
    s.Colors[ImGuiCol_SliderGrabActive] = s.Colors[ImGuiCol_NavCursor] = accent;
    s.Colors[ImGuiCol_TextSelectedBg] = {accent.x, accent.y, accent.z, 0.35f};
    s.Colors[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    s.Colors[ImGuiCol_ScrollbarGrab] = {0.24f, 0.29f, 0.32f, 1};
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = accent;
    s.Colors[ImGuiCol_ScrollbarGrabActive] = primaryColor;
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
void steamChallenge(Form &form, const Snapshot::Authentication &auth, float width) {
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
    if (form.provider == Provider::epic) {
        if (!matching || !auth.busy) {
            if (button(matching && !auth.identity.empty() ? "Reconnect Epic" : "Sign in to Epic", 0,
                    primaryColor))
                form.request({.action = Action::connect, .id = form.id, .provider = Provider::epic});
            return;
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextWrapped("%s", auth.prompt.c_str());
        ImGui::PopTextWrapPos();
        if (iconButton(Icon::close, "Cancel sign-in")) form.request({.action = Action::cancelConnect});
        return;
    }
    if (matching && auth.busy) {
        steamChallenge(form, auth, width);
        return;
    }
    if (form.steamPassword) {
        rowField("Steam", form.email, width, "Account name");
        rowField("Password", form.password, width, "Steam password",
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo);
    }
    if (button(
            matching && !auth.identity.empty() ? "Reconnect Steam" : "Sign in to Steam", 0, primaryColor)) {
        Command command{.action = Action::connect, .id = form.id, .provider = form.provider};
        if (form.steamPassword) {
            command.email = form.email.data();
            command.password = utf16(form.password.data());
        }
        form.request(std::move(command));
        wipe(form.password.data(), form.password.size());
        clearInputMemory();
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
void removeAccount(Form &form) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Remove?");
    ImGui::SameLine();
    if (iconButton(Icon::remove, "Confirm removal")) form.request({.action = Action::remove, .id = form.id});
    ImGui::SameLine();
    if (iconButton(Icon::close, "Cancel removal")) form.removing = false;
}
void accountFields(const Account *account, App &app, Form &form, const Snapshot &state, float width) {
    form.edited |= rowField("Name", form.label, width, "Account name");
    const char *providers[]{"ArenaNet", "Steam", "Epic"};
    float labels{};
    for (auto provider : providers)
        labels += ImGui::CalcTextSize(provider).x;
    const auto padding = (width - labels - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2{4 * ImGui::GetStyle().FontScaleDpi, ImGui::GetStyle().FramePadding.y});
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        if (!button(providers[i], ImGui::CalcTextSize(providers[i]).x + padding,
                static_cast<int>(form.provider) == i ? primaryColor : ImVec4{}) ||
            static_cast<int>(form.provider) == i)
            continue;
        form.request({.action = Action::cancelConnect});
        form.provider = static_cast<Provider>(i);
        form.edited = true;
        form.email.fill(0);
        wipe(form.password.data(), form.password.size());
        clearInputMemory();
    }
    ImGui::PopStyleVar();
    if (form.provider == Provider::arenaNet) {
        form.edited |= rowField("Email", form.email, width, form.loading ? "Opening…" : "ArenaNet email");
        form.edited |= rowField("Password", form.password, width, form.id.empty() ? "Password" : "Keep saved",
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo);
    } else {
        platformFields(form, state.auth, width);
    }
    form.edited |= rowField("Arguments", form.args, width, "e.g. -windowed -loadmapinfo");
    help("Leave empty to use global arguments; matching arguments here override them.");
    if (account && form.removing) {
        removeAccount(form);
        return;
    }
    if (!account) {
        ImGui::BeginDisabled(!form.canSave(state));
        if (iconButton(Icon::check, "Add account", true)) form.save(app, state.catalog);
        ImGui::EndDisabled();
        return;
    }
    form.removing = iconButton(Icon::remove, "Remove account");
}

void accountRow(const Account *account, App &app, Form &form, const Snapshot &state) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto row = ImGui::GetCursorScreenPos();
    const auto width = ImGui::GetContentRegionAvail().x;
    const float header = 56 * scale, inset = 12 * scale;
    static const SessionView ready{{}, "Ready"};
    const auto &id = account ? account->id : ready.id;
    const auto found = state.session(id);
    const auto &session = found ? *found : ready;
    const bool registration = account && session.active && session.state == 9;
    bool expanded = registration || (form.page == Form::Page::account && form.id == id);
    const bool busy = form.pending || form.picker || state.busy;
    ImGui::PushID(account ? account->id.c_str() : "new-account");
    auto storage = ImGui::GetStateStorage();
    const auto revealId = ImGui::GetID("reveal"), heightId = ImGui::GetID("height");
    const float reveal = std::clamp(
        storage->GetFloat(revealId) + (expanded ? 1 : -1) * ImGui::GetIO().DeltaTime / 0.16f, 0.0f, 1.0f);
    storage->SetFloat(revealId, reveal);
    form.rowsMoving |= reveal != (expanded ? 1 : 0);
    const float body = storage->GetFloat(heightId) * reveal * reveal * (3 - 2 * reveal);
    const auto total = header + body;
    if (!ImGui::IsRectVisible({width, total}) && !expanded) {
        ImGui::Dummy({width, total});
        ImGui::PopID();
        return;
    }
    const bool selected = account && form.isSelected(id);
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(row, {row.x + width, row.y + total},
        ImGui::GetColorU32(selected ? ImGui::GetStyleColorVec4(ImGuiCol_Header)
                                    : ImVec4{20 / 255.f, 24 / 255.f, 29 / 255.f, 1}),
        7 * scale);
    if (selected) draw->AddRect(row, {row.x + width, row.y + total}, ImGui::GetColorU32(accent), 7 * scale);
    const ImVec2 pos{row.x + inset, row.y + 8 * scale};
    const auto available = width - 2 * inset;
    const auto textWidth = available - (account ? 72 : 36) * scale;
    ImGui::SetCursorScreenPos(pos);
    clippedText(account ? account->label.c_str() : "Add account", textWidth);
    help(account ? account->label.c_str() : "Add account");
    const auto statusY = pos.y + ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorScreenPos({pos.x, statusY});
    char pid[32]{};
    if (state.catalog.showPid && session.active && session.pid)
        std::snprintf(pid, sizeof(pid), " · %u", session.pid);
    const auto pidWidth = ImGui::CalcTextSize(pid).x;
    const bool error = session.state == 6 ||
        (!session.active && session.status != "Ready" && session.status != "Client exited");
    const auto status = !account ? "Choose how you sign in"
        : error                  ? "Needs attention"
                                 : session.status.c_str();
    const auto statusWidth = std::min(ImGui::CalcTextSize(status).x, textWidth - pidWidth);
    ImGui::PushStyleColor(ImGuiCol_Text, error ? errorColor : session.active ? accent : muted);
    clippedText(status, statusWidth);
    ImGui::PopStyleColor();
    help(session.status.c_str());
    if (pidWidth) {
        ImGui::SetCursorScreenPos({pos.x + statusWidth, statusY});
        mutedText(pid);
    }
    const ImVec2 actions{pos.x + available - 64 * scale, row.y + (header - 28 * scale) / 2};
    ImGui::SetCursorScreenPos(actions);
    if (account) {
        ImGui::BeginDisabled(session.active ? !session.canClose() || form.pending : busy);
        if (iconButton(session.active || expanded ? Icon::close : Icon::edit,
                session.active ? "Close game"
                    : expanded ? "Close editor"
                               : "Edit account")) {
            if (session.active)
                form.request({.action = Action::close, .id = id});
            else if (expanded)
                form.go(Form::Page::accounts);
            else
                form.go(Form::Page::account, id);
            form.rowsMoving = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(session.active ? !session.canShow : busy || expanded);
        if (iconButton(
                session.active ? Icon::show : Icon::play, session.active ? "Show game" : "Launch account")) {
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
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
    } else {
        ImGui::SetCursorScreenPos({pos.x + available - 28 * scale, actions.y});
        ImGui::BeginDisabled(busy);
        if (iconButton(Icon::close, "Cancel new account")) form.go(Form::Page::accounts);
        ImGui::EndDisabled();
    }
    expanded = registration || (form.page == Form::Page::account && form.id == id);
    if (expanded) {
        auto window = ImGui::GetCurrentWindow();
        const auto measured = window->DC.CursorMaxPos;
        ImGui::PushClipRect({row.x, row.y + header}, {row.x + width, row.y + total}, true);
        ImGui::SetCursorScreenPos({pos.x, row.y + header + 4 * scale});
        ImGui::BeginGroup();
        ImGui::BeginDisabled(form.pending || reveal < 1 || (!registration && busy));
        if (registration)
            registrationFields(form, id, available);
        else
            accountFields(account, app, form, state, available);
        ImGui::EndDisabled();
        ImGui::EndGroup();
        storage->SetFloat(heightId, ImGui::GetItemRectMax().y - (row.y + header) + 10 * scale);
        ImGui::PopClipRect();
        // Measure the complete form, but publish only its revealed height to the list.
        // This keeps animation from creating a second scrolling region or a premature scroll extent.
        window->DC.CursorMaxPos = measured;
    }
    ImGui::SetCursorScreenPos(row);
    ImGui::Dummy({width, total});
    ImGui::PopID();
}

void header(Form &form, SDL_Window *window, bool busy) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto width = ImGui::GetWindowWidth();
    ImGui::SetCursorPos({14 * scale, (headerHeight * scale - ImGui::GetTextLineHeight()) / 2});
    ImGui::TextUnformatted("GW2");
    ImGui::SameLine();
    mutedText("Multi");
    ImGui::SetCursorPos(headerButton(0, width, scale).Min);
    ImGui::BeginDisabled(busy);
    if (iconButton(form.page != Form::Page::settings ? Icon::settings : Icon::back,
            form.page != Form::Page::settings ? "Settings" : "Back to accounts")) {
        form.go(form.page != Form::Page::settings ? Form::Page::settings : Form::Page::accounts);
    }
    ImGui::EndDisabled();
    ImGui::SetCursorPos(headerButton(1, width, scale).Min);
    if (iconButton(Icon::minimize, "Minimize")) SDL_MinimizeWindow(window);
    ImGui::SetCursorPos(headerButton(2, width, scale).Min);
    if (iconButton(Icon::close, "Close launcher; keep games running")) form.go(Form::Page::exit);
    ImGui::SetCursorPos({14 * scale, headerHeight * scale});
    ImGui::Separator();
    ImGui::SetCursorPos({14 * scale, (headerHeight + 8) * scale});
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N) && !busy) form.go(Form::Page::account);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma) && !busy) form.go(Form::Page::settings);
    if (form.page != Form::Page::accounts && ImGui::IsKeyPressed(ImGuiKey_Escape) && !busy)
        form.go(Form::Page::accounts);
}
void toolbar(Form &form, const Snapshot &state, bool busy) {
    if (form.page == Form::Page::settings) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Settings");
        ImGui::Spacing();
        return;
    }
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const auto page = form.page;
    const bool closeAll = std::ranges::any_of(state.sessions, &SessionView::canClose);
    const auto actionCount = 2 + closeAll;
    const auto width = ImGui::GetContentRegionAvail().x;
    const auto actionsWidth = (iconSize * actionCount + iconSpacing * (actionCount - 1)) * scale;
    const auto pos = ImGui::GetCursorScreenPos();
    const auto actionsX = ImGui::GetCursorPosX() + width - actionsWidth;
    ImGui::PushClipRect(pos, {pos.x + width - actionsWidth - 8 * scale, pos.y + 28 * scale}, true);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Accounts");
    ImGui::PopClipRect();
    ImGui::SameLine(actionsX);
    ImGui::BeginDisabled(busy);
    if (iconButton(Icon::add, "Add account")) form.go(Form::Page::account);
    ImGui::SameLine(0, iconSpacing * scale);
    const bool editingRequested = page == Form::Page::account && !form.id.empty() &&
        (form.selected.empty() || form.isSelected(form.id));
    ImGui::BeginDisabled(state.catalog.accounts.empty() || editingRequested);
    if (iconButton(Icon::play, form.selected.empty() ? "Launch all" : "Launch selected"))
        form.launch(state.catalog);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (closeAll) {
        ImGui::SameLine(0, iconSpacing * scale);
        ImGui::BeginDisabled(busy);
        if (iconButton(Icon::closeAll, "Close all games (including unselected accounts)"))
            form.request({.action = Action::closeAll});
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
}
template <std::size_t N>
void pathField(Form &form, SDL_Window *window, const char *label, std::array<char, N> &value, unsigned target,
    bool folder, const char *hint) {
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    mutedText(label);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 36 * scale);
    form.edited |= ImGui::InputTextWithHint("##path", hint, value.data(), value.size());
    ImGui::SameLine();
    if (iconButton(Icon::folder, "Browse"))
        form.choose(window, target, folder, value[0] ? value.data() : nullptr);
    ImGui::PopID();
}
void settingsPage(Form &form, SDL_Window *window) {
    pathField(form, window, "Game executable", form.game, 1, false, "Full path to Gw2-64.exe");
    form.edited |= field("Global arguments", form.args, "e.g. -windowed -loadmapinfo");
    form.edited |= ImGui::Checkbox("Hide sign-in window", &form.hide);
    help("GW2 opens when sign-in needs attention. Game windows are always shown.");
    form.edited |= ImGui::Checkbox("Show PID", &form.showPid);
#ifndef _WIN32
    ImGui::Spacing();
    ImGui::SeparatorText("Wine / Proton");
    form.edited |= field("Runner", form.runner, "wine, umu-run, or an absolute path");
    pathField(form, window, "Wine prefix", form.prefix, 2, true, "Directory containing system.reg");
    if (path(form.runner.data()).filename() == "umu-run")
        pathField(form, window, "Proton directory", form.proton, 3, true,
            "Directory containing the proton executable");
#endif
}

void render(App &app, Form &form, const Snapshot &state, SDL_Window *window, bool &open) {
    form.observe(state);
    form.observePicker();
    form.rowsMoving = false;
    const auto scale = ImGui::GetStyle().FontScaleDpi;
    const bool busy = form.pending || form.picker || state.busy || !state.ready;
    auto vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::Begin("GW2 Multi Launcher", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse);
    header(form, window, busy);
    const auto message = form.localError.empty() ? state.error : form.localError;
    const auto page = form.page;
    const bool accounts = page != Form::Page::settings;
    toolbar(form, state, busy);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * form.opacity());
    // Each page owns its scroll position; navigation never reuses another form's scroll offset.
    ImGui::PushID(accounts ? 0 : 1);
    if (form.scrollToNew) {
        ImGui::SetNextWindowScroll({0, 0});
        form.scrollToNew = false;
    }
    ImGui::BeginChild("page");
    if (!message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, errorColor);
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    if (!state.ready) {
        mutedText(state.fatal ? "Your saved accounts have not been changed." : "Opening accounts…");
    } else if (accounts) {
        if (const auto update = state.session(""); update && update->active) {
            mutedText(update->status.c_str());
            ImGui::Spacing();
        }
        if (state.catalog.accounts.empty() && form.page != Form::Page::account) {
            ImGui::Dummy({0, 20 * scale});
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted("Your accounts, one click away.");
            mutedText("Add an ArenaNet account to get started.");
            ImGui::PopTextWrapPos();
        }
        if (form.page == Form::Page::account && form.id.empty()) accountRow(nullptr, app, form, state);
        for (const auto &account : state.catalog.accounts)
            accountRow(&account, app, form, state);
    } else {
        ImGui::BeginDisabled(busy || form.page != page);
        settingsPage(form, window);
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::PopID();
    ImGui::PopStyleVar();
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
    glClearColor(background.x, background.y, background.z, background.w);
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
    if (SDL_WaitEventTimeout(
            &event, io.WantTextInput || io.MouseDown[0] || form.transitioning() || tooltipPending ? 8 : 125))
        do {
            ImGui_ImplSDL3_ProcessEvent(&event);
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
        SDL_CreateWindow("GW2 Multi Launcher", static_cast<int>(360 * scale), static_cast<int>(480 * scale),
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow);
    if (!window) throw std::runtime_error(SDL_GetError());
    const auto base = SDL_GetBasePath();
    if (!base) throw std::runtime_error(SDL_GetError());
    const auto iconFile = std::string(base) + "gw2-multi-launcher.bmp";
    auto icon = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
        SDL_LoadBMP(iconFile.c_str()), SDL_DestroySurface);
    if (!icon || !SDL_SetWindowIcon(window.get(), icon.get())) throw std::runtime_error(SDL_GetError());
    SDL_SetWindowPosition(window.get(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_SetWindowHitTest(window.get(), hitTest, nullptr);
    return window;
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
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "none");
    SDL_SetHint(SDL_HINT_APP_NAME, "GW2 Multi Launcher");
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
    setupFonts(scale);
    if (!(gui.platform = ImGui_ImplSDL3_InitForOpenGL(window.get(), context)) ||
        !(gui.renderer = ImGui_ImplOpenGL3_Init("#version 130")))
        throw std::runtime_error("Could not initialize the launcher renderer.");
    App app;
    Form form;
    bool open = true;
    bool restored{};
    WindowState observed;
    while (open) {
        if (!pollEvents(app, form, window.get(), context)) form.go(Form::Page::exit);
        const auto snapshot = app.snapshot();
        if (!restored && (snapshot->ready || snapshot->fatal)) {
            observed = snapshot->catalog.window;
            restoreWindow(window.get(), observed);
            SDL_ShowWindow(window.get());
            restored = true;
        }
        const auto flags = SDL_GetWindowFlags(window.get());
        if (restored && snapshot->ready && !(flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED))) {
            WindowState current;
            int width{}, height{};
            SDL_GetWindowSize(window.get(), &width, &height);
            current.scale = ImGui::GetStyle().FontScaleDpi;
            current.width = static_cast<int>(std::lround(width / current.scale));
            current.height = static_cast<int>(std::lround(height / current.scale));
            current.positioned = SDL_GetWindowPosition(window.get(), &current.x, &current.y);
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
        drawFrame(app, form, *snapshot, window.get(), context, open);
    }
    form.close();
    return 0;
}
} // namespace gw2

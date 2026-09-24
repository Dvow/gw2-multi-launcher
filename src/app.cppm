module;
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

export module app;
import game;
import platform;
import accounts;
import session;
import epic;
import update;

export namespace gw2 {
enum class Action {
    settings,
    save,
    edit,
    remove,
    reorder,
    launch,
    show,
    close,
    closeAll,
    checkUpdate,
    installUpdate,
    connect,
    guard,
    verify,
    cancelConnect,
    displayName,
    cancelSetup
};
struct Command {
    Action action{};
    std::uint64_t serial{};
    std::string id{}, label{}, email{}, arguments{}, beforeId{};
    std::vector<std::string> ids{};
    std::vector<std::string> dlls{};
    Secret password{};
    unsigned pid{};
    Provider provider{};
    Catalog settings{};
};
struct SessionView {
    std::string id, status;
    std::string email{};
    unsigned state{}, pid{}, verification{};
    bool active{}, canShow{};
    bool needsVerification() const { return active && (state == 12 || state == 13); }
    bool canClose() const { return active && (state == 4 || needsVerification()) && !id.empty(); }
    bool blocking() const {
        return active && (id.empty() || state < 4 || state == 9 || state == 10 || state == 11 || needsVerification());
    }
};
struct Snapshot {
    struct Authentication {
        std::string id, qr, prompt, identity, username;
        bool busy{}, code{};
        Provider provider{Provider::steam};
        bool connected{};
    } auth;
    Catalog catalog;
    AppUpdate update;
    std::vector<SessionView> sessions;
    std::string error, editId, editEmail;
    std::uint64_t completed{};
    bool ready{}, busy{}, fatal{};
    const SessionView *session(std::string_view id) const {
        const auto found = std::ranges::find(sessions, id, &SessionView::id);
        return found == sessions.end() ? nullptr : &*found;
    }
};

class App {
    static constexpr std::uint32_t magic = 0x36585747;
    struct Session {
        SessionView view;
        std::unique_ptr<Process> process;
        Secret outgoing;
        std::size_t written{}, received{};
        std::array<unsigned char, 12> record{};
        std::uint64_t deadline{};
        bool show{}, failed{}, closeRequested{};
    };
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Command> commands_;
    std::optional<WindowState> window_;
    std::uint64_t windowDue_{};
    std::uint64_t serial_{};
    std::shared_ptr<const Snapshot> published_{std::make_shared<Snapshot>()};
    Snapshot state_;
    std::unique_ptr<Store> store_;
    std::vector<Session> sessions_;
    std::deque<std::string> launches_;
    std::uint32_t wantedBuild_{};
    std::unique_ptr<SteamConnection> auth_;
    std::optional<EpicConnection> epic_;
    Secret connectedSession_;
    std::future<AppUpdate> updateTask_;
    std::jthread worker_;

    void publish() {
        state_.auth.connected = !connectedSession_.bytes.empty();
        state_.sessions.clear();
        for (const auto &s : sessions_)
            state_.sessions.push_back(s.view);
        state_.busy = !launches_.empty() || blocked() || state_.update.stage == UpdateStage::installing ||
            state_.update.stage == UpdateStage::installed;
        if (store_) state_.catalog = store_->catalog();
        {
            std::lock_guard lock(mutex_);
            published_ = std::make_shared<Snapshot>(state_);
        }
        SDL_Event event{};
        event.type = SDL_EVENT_USER;
        SDL_PushEvent(&event);
    }
    bool blocked() const {
        return std::ranges::any_of(sessions_, [](const Session &s) { return s.view.blocking(); });
    }
    bool running() const {
        return std::ranges::any_of(sessions_, [](const Session &s) { return s.view.active; });
    }
    void clearAuthentication() {
        auth_.reset();
        epic_.reset();
        connectedSession_.clear();
        state_.auth = {};
    }
    bool active(std::string_view id) const {
        return std::ranges::any_of(
            sessions_, [&](const Session &s) { return s.view.id == id && s.view.active; });
    }
    static std::string failure(std::uint32_t code) {
        if ((code & 0xFF000000u) == 0x40000000u)
            return "DLL " + std::to_string((code >> 16) & 0xFF) +
                " could not be copied or loaded (Windows error " + std::to_string(code & 0xFFFF) +
                "). Check the file, temporary-folder access, its 64-bit dependencies, and initialization. "
                "Close this client before retrying.";
        switch (code) {
        case 1001:
            return "Wine could not resolve the game path.";
        case 1002:
            return "Close clients opened outside this launcher, then try again.";
        case 1003:
            return "GW2 needs attention. Select Show to open its window.";
        case 1004:
            return "Sign-in needs attention. Select Show; no automatic retry was sent.";
        case 1005:
            return "Close every GW2 client before updating, and check that Gw2.dat is writable.";
        case 1006:
            return "The official GW2 updater did not finish. Launch again to resume it.";
        case 1007:
            return "GW2's platform connection could not be prepared. Close the game and try again.";
        case 1008:
            return "GW2 could not receive the close request. Select Show and close its window.";
        case 258:
            return "GW2 timed out. Select Show to check its window.";
        case 1103:
            return "GW2's native interface has changed. Update the launcher before signing in.";
        case 1010:
            return "Steam could not authenticate this account. Edit it and sign in again.";
        case 1011:
            return "Add the free Guild Wars 2 game to this Steam account's library, then launch again.";
        case 1012:
            return "Steam returned a ticket larger than GW2's native login accepts. Update the launcher.";
        case 1013:
            return "The saved Steam session expired or was revoked. Edit this account and reconnect Steam.";
        case 1014:
            return "Steam returned a different account identity. Reconnect this saved account.";
        default:
            if (code > 20000 && code < 85536)
                return "GW2 sign-in returned error " + std::to_string(code - 20000) +
                    ". Select Show to check its window.";
            return "The game helper reported error " + std::to_string(code) +
                ". Check its window before retrying.";
        }
    }
    Secret epicCredentials(const std::string &id, std::string_view name, std::stop_token stop) {
        const auto account = store_->account(id);
        auto saved = openAccount(account, stop);
        cancelled(stop);
        auto login = epicRefresh(saved, path(store_->catalog().gamePath), {});
        // Refresh tokens rotate. Commit the replacement before launching the game,
        // so cancellation or a launch failure cannot discard the saved session.
        store_->put(
            id, account.label, {}, account.arguments, account.dlls, Secret{}, {}, Provider::epic,
            std::move(login.record));
        cancelled(stop);
        Secret request;
        tokenCredentials(request.bytes, name, login.access.text());
        return request;
    }
    void start(std::string id, std::stop_token stop) {
        const bool update = id.empty();
        if (active(id)) throw std::runtime_error("This account already has a client.");
        Secret epicRequest;
        if (!update && store_->account(id).provider == Provider::epic)
            epicRequest = epicCredentials(id, "1", stop);
        const auto &catalog = store_->catalog();
        const auto dlls = update ? std::vector<std::string>{} : launchDlls(store_->account(id), catalog);
        Session session;
        session.view = {.id = id, .status = update ? "Starting updater…" : "Starting…", .active = true};
        auto &bytes = session.outgoing.bytes;
        const auto steam = !update && store_->account(id).provider == Provider::steam;
        if (steam) {
            const auto &account = store_->account(id);
            session.outgoing = steamRequest(catalog, account, openAccount(account, stop), dlls);
#ifdef _WIN32
            constexpr auto executable = "GW2MultiLauncher.exe";
#else
            constexpr auto executable = "GW2MultiLauncher";
#endif
            session.process = std::make_unique<Process>(
                std::vector<std::string>{utf8(installed(executable)), "--steam-session"},
                std::vector<std::pair<std::string, std::string>>{});
        } else {
            if (update) {
                appendNumber(bytes, magic);
                appendNumber(bytes, 1);
                wireString(bytes, catalog.gamePath);
            } else {
                const auto &account = store_->account(id);
                gameRequest(bytes, catalog, account, dlls);
                auto secret =
                    account.provider == Provider::epic ? std::move(epicRequest) : openAccount(account, stop);
                if (account.provider == Provider::arenaNet)
                    session.view.email = fromUtf16(
                        std::span(secret.bytes).subspan(8, (number(secret.bytes) + 1) * 2));
                // Reserve before copying the password, so vector growth cannot leave
                // abandoned plaintext allocations behind.
                bytes.reserve(bytes.size() + 5 + secret.bytes.size());
                bytes.push_back(3);
                if (account.provider != Provider::epic)
                    appendNumber(bytes, static_cast<std::uint32_t>(secret.bytes.size()));
                bytes.insert(bytes.end(), secret.bytes.begin(), secret.bytes.end());
            }
            cancelled(stop);
            session.process = gameRunner(catalog, dllDirectories(dlls));
        }
        session.deadline = SDL_GetTicks() + 90000;
        std::erase_if(sessions_,
            [&](const Session &existing) { return existing.view.id == id && !existing.view.active; });
        sessions_.push_back(std::move(session));
        publish();
    }
    void prepare(std::stop_token stop);
    void saveAccount(Command &command, std::stop_token stop) {
        if (active(command.id)) throw std::runtime_error("Close this account's client before editing it.");
        if (state_.auth.busy) throw std::runtime_error("Finish signing in before saving.");
        if (!connectedSession_.bytes.empty() &&
            (state_.auth.id != command.id || state_.auth.provider != command.provider))
            throw std::runtime_error("Sign in from this account's editor before saving.");
        // Saving may fail validation or disk I/O. Keep the verified
        // draft session until the catalog has actually been committed.
        Secret candidate;
        candidate.bytes.assign(connectedSession_.bytes.begin(), connectedSession_.bytes.end());
        if (command.provider != Provider::arenaNet && trim(command.label).empty())
            command.label = state_.auth.username;
        const auto id = command.id;
        store_->put(std::move(command.id), std::move(command.label), std::move(command.email),
            std::move(command.arguments), std::move(command.dlls), std::move(command.password), stop,
            command.provider, std::move(candidate));
        clearAuthentication();
        if (id.empty()) return;
        const auto &account = store_->account(id);
        state_.auth = {id, {}, {}, account.identity, account.username, false, false, account.provider};
    }
    void editAccount(Command &command, std::stop_token stop) {
        if (active(command.id)) throw std::runtime_error("Close this account's client before editing it.");
        clearAuthentication();
        const auto &account = store_->account(command.id);
        auto secret = openAccount(account, stop);
        if (account.provider == Provider::arenaNet)
            state_.editEmail = fromUtf16(std::span(secret.bytes).subspan(8, (number(secret.bytes) + 1) * 2));
        else
            state_.auth = {command.id, {}, {}, account.identity, account.username};
        state_.auth.provider = account.provider;
        state_.editId = command.id;
    }
    void queueLaunch(Command &command, std::stop_token stop) {
        if (command.ids.empty()) throw std::runtime_error("Select an account to launch.");
        // Resolve every requested identity before preparing any client. A stale or empty
        // selection must never fall back to launching all accounts.
        for (const auto &id : command.ids)
            (void)store_->account(id);
        for (const auto &a : store_->catalog().accounts)
            if (std::ranges::find(command.ids, a.id) != command.ids.end() && !active(a.id))
                launches_.push_back(a.id);
        if (launches_.empty()) throw std::runtime_error("The requested accounts are already running.");
        prepare(stop);
    }
    void setupAccount(Command &command, std::stop_token stop) {
        const bool cancel = command.action == Action::cancelSetup;
        if (!cancel && !validDisplayName(command.label))
            throw std::runtime_error("Use 3–27 letters and single spaces for your GW2 display name.");
        auto found = std::ranges::find_if(sessions_,
            [&](const Session &s) { return s.view.id == command.id && s.view.active && s.view.state == 9; });
        if (found == sessions_.end() || !found->outgoing.bytes.empty() ||
            store_->account(command.id).provider == Provider::arenaNet)
            throw std::runtime_error("This account is not waiting for a GW2 display name.");
        Secret retry;
        const bool epic = store_->account(command.id).provider == Provider::epic;
        if (!cancel && epic) retry = epicCredentials(command.id, command.label, stop);
        auto &bytes = found->outgoing.bytes;
        bytes.reserve(1 + retry.bytes.size() + command.label.size() + 4);
        bytes.push_back(cancel ? 4 : 3);
        if (cancel)
            launches_.clear();
        else if (epic)
            bytes.insert(bytes.end(), retry.bytes.begin(), retry.bytes.end());
        else
            textField(bytes, command.label);
        found->view.state = 2;
        found->view.status = cancel ? "Cancelling…" : "Creating GW2 account…";
        found->deadline = SDL_GetTicks() + 130000;
    }
    void connectSteam(Command &command) {
        if (command.provider != Provider::steam)
            throw std::runtime_error("This account provider is not available yet.");
        clearAuthentication();
        state_.auth = {command.id, {}, "Connecting to Steam…", {}, {}, true};
        auth_ = std::make_unique<SteamConnection>();
        if (command.password.bytes.empty())
            auth_->send(1);
        else {
            Secret fields;
            fields.bytes.reserve(8192);
            textField(fields.bytes, command.email);
            auto password = fromUtf16(command.password.bytes);
            textField(fields.bytes, password);
            wipe(password.data(), password.size());
            auth_->send(2, fields.bytes);
        }
    }
    void verify(Command &command) {
        auto found = std::ranges::find_if(sessions_, [&](const Session &s) {
            return s.view.id == command.id && s.view.active && s.view.state == 12 &&
                s.view.pid == command.pid && !s.closeRequested;
        });
        if (found == sessions_.end() || !found->outgoing.bytes.empty())
            throw std::runtime_error("This verification request has ended. Check the account's status.");
        const auto &code = command.password.bytes;
        const auto digits = found->view.verification == 6 ? 6u : 5u;
        if ((found->view.verification != 1 && found->view.verification != 5 && found->view.verification != 6) ||
            code.size() != (digits + 1) * 2 || code[code.size() - 2] || code.back())
            throw std::runtime_error("Enter the complete verification code.");
        for (std::size_t i = 0; i < code.size() - 2; i += 2)
            if (code[i] < '0' || code[i] > '9' || code[i + 1])
                throw std::runtime_error("The verification code must contain only digits.");
        auto &bytes = found->outgoing.bytes;
        bytes.reserve(5 + code.size());
        bytes.push_back(6);
        appendNumber(bytes, static_cast<std::uint32_t>(code.size()));
        bytes.insert(bytes.end(), code.begin(), code.end());
        found->view.state = 13;
        found->view.status = "Checking verification code…";
        found->deadline = SDL_GetTicks() + 130000;
    }
    void closeSession(Session &session) {
        if (!session.view.canClose() || !session.outgoing.bytes.empty()) return;
        session.outgoing.bytes.push_back(5);
        session.closeRequested = true;
        session.show = false;
    }
    void connectEpic(Command &command, std::stop_token stop) {
        clearAuthentication();
        state_.auth = {command.id, {}, "Connecting to Epic…", {}, {}, true, false, Provider::epic};
        publish();
        epic_.emplace(path(store_->catalog().gamePath), stop);
        if (!SDL_OpenURL(epic_->url().c_str()))
            throw std::runtime_error("Could not open the Epic sign-in page.");
        state_.auth.prompt = "Finish signing in and approve Guild Wars 2 in your browser. "
                             "Your account will appear here automatically.";
    }
    void dispatch(Command &command, std::stop_token stop) {
        switch (command.action) {
        case Action::settings: {
            if (running() && command.settings.gamePath != store_->catalog().gamePath)
                throw std::runtime_error("Close your clients before changing the game installation.");
            const bool check = command.settings.autoUpdate && !store_->catalog().autoUpdate;
            store_->settings(std::move(command.settings));
            if (check) beginUpdate(false, stop);
            break;
        }
        case Action::save:
            saveAccount(command, stop);
            break;
        case Action::remove:
            if (active(command.id))
                throw std::runtime_error("Close this account's client before removing it.");
            store_->remove(command.id);
            break;
        case Action::edit:
            editAccount(command, stop);
            break;
        case Action::reorder:
            store_->reorder(command.id, command.beforeId);
            break;
        case Action::launch:
            queueLaunch(command, stop);
            break;
        case Action::checkUpdate:
        case Action::installUpdate:
            beginUpdate(command.action == Action::installUpdate, stop);
            break;
        case Action::show:
            for (auto &session : sessions_)
                if (session.view.id == command.id && session.view.active) session.show = true;
            break;
        case Action::close:
        case Action::closeAll:
            for (auto &session : sessions_)
                if (command.action == Action::closeAll || session.view.id == command.id)
                    closeSession(session);
            break;
        case Action::displayName:
        case Action::cancelSetup:
            setupAccount(command, stop);
            break;
        case Action::cancelConnect:
            clearAuthentication();
            break;
        case Action::verify:
            verify(command);
            break;
        case Action::connect:
            if (command.provider == Provider::epic)
                connectEpic(command, stop);
            else
                connectSteam(command);
            break;
        case Action::guard: {
            if (!auth_ || !state_.auth.code || state_.auth.id != command.id)
                throw std::runtime_error("This Steam Guard request has ended. Sign in again.");
            Secret fields;
            textField(fields.bytes, command.email);
            auth_->send(4, fields.bytes);
            state_.auth.code = false;
            state_.auth.prompt = "Checking Steam Guard…";
            break;
        }
        }
    }
    void execute(Command command, std::stop_token stop) {
        state_.error.clear();
        state_.editId.clear();
        state_.editEmail.clear();
        try {
            if (command.action != Action::show && command.action != Action::close &&
                command.action != Action::guard && command.action != Action::verify && command.action != Action::cancelConnect &&
                command.action != Action::displayName && command.action != Action::cancelSetup &&
                command.action != Action::checkUpdate && state_.busy)
                throw std::runtime_error("Wait for the current launch or update to finish.");
            dispatch(command, stop);
        } catch (const std::exception &e) {
            if (command.action == Action::connect) {
                auth_.reset();
                epic_.reset();
                state_.auth.busy = false;
            }
            state_.error = e.what();
            launches_.clear();
        }
        state_.completed = command.serial;
        publish();
    }
    bool pollAuthentication(std::stop_token stop) {
        if (!auth_ && !epic_) return false;
        try {
            if (epic_) {
                auto login = epic_->poll(stop);
                if (!login) return false;
                const auto id = state_.auth.id;
                if (!id.empty() && store_->account(id).provider == Provider::epic &&
                    login->identity != store_->account(id).identity)
                    throw std::runtime_error("You signed in to a different Epic account. Reconnect with "
                                             "this account, or add a separate account instead.");
                state_.auth = {id, {}, {}, login->identity, login->username, false, false, Provider::epic};
                connectedSession_ = std::move(login->record);
                epic_.reset();
                return true;
            }
            auto message = auth_->poll();
            if (!message) return false;
            Fields fields(std::span(message->data.bytes).subspan(4));
            switch (message->kind) {
            case 1: {
                auto qr = fields.next();
                fields.end();
                if (qr.empty() || qr.size() > 32761 ||
                    !std::ranges::all_of(qr, [](char c) { return c == '0' || c == '1'; }))
                    throw std::runtime_error("Invalid Steam QR code.");
                state_.auth.qr = qr;
                state_.auth.prompt = "Scan with the Steam mobile app.";
                break;
            }
            case 2:
            case 6:
                state_.auth.prompt = fields.next();
                fields.end();
                state_.auth.code = message->kind == 2;
                break;
            case 3: {
                const auto identity = fields.next(), username = fields.next(), token = fields.next();
                fields.end();
                if (identity.size() != 17 ||
                    !std::ranges::all_of(identity, [](char c) { return c >= '0' && c <= '9'; }) ||
                    username.empty() || username.size() > 64 || token.empty() || token.size() > 8192)
                    throw std::runtime_error("Steam returned an invalid account session.");
                state_.auth.identity = identity;
                state_.auth.username = username;
                const auto record = std::span(message->data.bytes).subspan(4);
                connectedSession_.bytes.assign(record.begin(), record.end());
                state_.auth.busy = state_.auth.code = false;
                state_.auth.prompt.clear();
                state_.auth.qr.clear();
                auth_.reset();
                break;
            }
            case 5: {
                const std::string error(fields.next());
                (void)fields.next();
                fields.end();
                throw std::runtime_error(error);
            }
            default:
                throw std::runtime_error("Unexpected Steam helper response.");
            }
        } catch (const std::exception &e) {
            state_.error = e.what();
            state_.auth.busy = state_.auth.code = false;
            state_.auth.qr.clear();
            state_.auth.prompt.clear();
            auth_.reset();
            epic_.reset();
            connectedSession_.clear();
        }
        return true;
    }
    void receiveStatus(Session &session) {
        session.received = 0;
        if (number(session.record) != magic)
            throw std::runtime_error(
                "The Wine/Proton runner changed the helper protocol. Use wine or umu-run directly.");
        const auto state = number(session.record, 4), detail = number(session.record, 8);
        if (state < 1 || state > 13 || (session.view.id.empty() ? state < 5 || state > 8
                                        : state == 7 || state == 8))
            throw std::runtime_error("Unexpected game helper status.");
        if ((state == 12 || state == 13) && detail != 1 && detail != 2 && detail != 4 && detail != 5 && detail != 6)
            throw std::runtime_error("Unknown GW2 verification request. Select Show to check the game.");
        if (state == 1) session.view.pid = detail;
        if (state == 5 || state == 8) session.view.pid = 0;
        session.view.state = session.failed && state != 5 ? 6 : state;
        session.view.verification = state == 12 || state == 13 ? detail : 0;
        if (state == 3 || state == 4 || state == 5 || state == 6) session.view.email.clear();
        if (state == 6) {
            session.failed = true;
            session.view.status = failure(detail);
            launches_.clear();
            state_.error = session.view.status;
        } else if (!session.failed) {
            constexpr const char *statuses[]{"", "Starting…", "Signing in…", "Opening game…", "Running",
                "Client exited", "", "Updating GW2…", "Updated", "Choose a GW2 display name",
                "Review GW2's agreement", "Loading DLLs…", "Verification required", "Checking verification code…"};
            session.view.status = statuses[state];
            if (state == 11) session.view.status = "Loading DLL " + std::to_string(detail) + "…";
            if (state == 12 && detail == 5) session.view.status = "Check your email";
        }
        if (state == 1 || state == 2 || state == 3 || state == 4 || state == 7 || state == 10 || state >= 11)
            session.view.canShow = true;
        if (state == 9) session.view.canShow = false;
        session.deadline = SDL_GetTicks() + (state == 2 || state == 13 ? 130000 : 100000);
        if (state == 8) {
            const Image image(path(store_->catalog().gamePath));
            if (image.build() < wantedBuild_)
                throw std::runtime_error("GW2's update did not reach the current build. Try again.");
            (void)image.resolve();
            store_->updatePending(false);
        }
        if (state == 5 || state == 8) {
            if (state == 5 && !session.failed && !session.closeRequested && !launches_.empty())
                throw std::runtime_error("A client exited before all accounts finished launching.");
            session.view.active = session.view.canShow = false;
            session.process.reset();
        }
    }
    bool poll(Session &session, std::stop_token stop) {
        bool changed{};
        try {
            if (!session.outgoing.bytes.empty()) {
                session.process->write(session.outgoing, session.written);
            } else if (session.show) {
                const unsigned char byte = 1;
                session.process->allowForeground();
                if (session.process->write({&byte, 1}) == 1) session.show = false;
            }
            // A bounded read makes runner noise, truncated records and inherited
            // pipes observable failures instead of an indefinitely busy account.
            session.received += session.process->read(std::span(session.record).subspan(session.received));
            if (session.received == session.record.size()) {
                receiveStatus(session);
                changed = true;
            }
            if (session.view.active && (session.view.state < 4 || session.view.state == 11 || session.view.state == 13) &&
                SDL_GetTicks() > session.deadline)
                throw std::runtime_error("The game helper timed out before opening the game. Check your "
                                         "runner and the GW2 window.");
            cancelled(stop);
        } catch (const std::exception &e) {
            if (!session.failed) {
                session.view.status = e.what();
                state_.error = e.what();
            }
            session.failed = true;
            session.view.active = session.view.canShow = false;
            session.view.pid = 0;
            session.view.email.clear();
            session.process.reset();
            session.outgoing.clear();
            launches_.clear();
            changed = true;
        }
        return changed;
    }
    bool saveWindow(bool flush = false) {
        std::optional<WindowState> pending;
        {
            std::lock_guard lock(mutex_);
            if (window_ && (flush || SDL_GetTicks() >= windowDue_)) pending.swap(window_);
        }
        if (!pending || !store_) return false;
        try {
            store_->window(*pending);
        } catch (const std::exception &e) {
            state_.error = e.what();
        }
        return true;
    }
    void beginUpdate(bool install, std::stop_token stop) {
        if (updateTask_.valid()) return;
        if (install && (running() || state_.auth.busy || !launches_.empty()))
            throw std::runtime_error("Close your games and finish signing in before updating the launcher.");
        if (install && state_.update.stage != UpdateStage::available)
            throw std::runtime_error("Check for updates before installing.");
        if (!install) state_.update = {};
        const auto release = state_.update;
        updateTask_ = std::async(std::launch::async, [release, install, stop] {
            if (!install) return checkAppUpdate(stop);
            installAppUpdate(release, stop);
            auto result = release;
            result.stage = UpdateStage::installed;
            return result;
        });
        state_.update.error.clear();
        state_.update.stage = install ? UpdateStage::installing : UpdateStage::checking;
    }
    bool pollUpdate() {
        if (!updateTask_.valid() ||
            updateTask_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
        try {
            state_.update = updateTask_.get();
        } catch (const std::exception &e) {
            state_.update.stage = state_.update.version.empty() ? UpdateStage::idle : UpdateStage::available;
            state_.update.error = e.what();
        }
        return true;
    }
    void run(std::stop_token stop) {
        try {
            store_ = std::make_unique<Store>();
            state_.ready = true;
            if (store_->catalog().autoUpdate) beginUpdate(false, stop);
            publish();
        } catch (const std::exception &e) {
            state_.error = e.what();
            state_.fatal = true;
            publish();
            return;
        }
        while (!stop.stop_requested()) {
            std::deque<Command> commands;
            {
                std::lock_guard lock(mutex_);
                commands.swap(commands_);
            }
            for (auto &command : commands) {
                if (stop.stop_requested()) break;
                execute(std::move(command), stop);
            }
            bool change = saveWindow();
            change |= pollUpdate();
            change |= pollAuthentication(stop);
            for (auto &session : sessions_)
                if (session.view.active) change |= poll(session, stop);
            if (!blocked() && !launches_.empty() && !stop.stop_requested()) {
                auto id = std::move(launches_.front());
                launches_.pop_front();
                try {
                    start(std::move(id), stop);
                } catch (const std::exception &e) {
                    state_.error = e.what();
                    launches_.clear();
                    change = true;
                }
            }
            if (change) publish();
            std::unique_lock lock(mutex_);
            if (!stop.stop_requested() && commands_.empty())
                changed_.wait_for(lock,
                    std::chrono::milliseconds((running() || auth_) ? 10
                            : window_ || updateTask_.valid()       ? 50
                                                                   : 1000));
        }
        saveWindow(true);
        if (updateTask_.valid()) updateTask_.wait();
        clearAuthentication();
        sessions_.clear();
        store_.reset();
    }

  public:
    App() : worker_([this](std::stop_token stop) { run(stop); }) {}
    ~App() {
        worker_.request_stop();
        changed_.notify_all();
        worker_.join();
    }
    App(const App &) = delete;
    App &operator=(const App &) = delete;
    void rememberWindow(WindowState value) {
        std::lock_guard lock(mutex_);
        window_ = value;
        windowDue_ = SDL_GetTicks() + 250; // Coalesce drag events; shutdown flushes the final size.
        changed_.notify_all();
    }
    std::shared_ptr<const Snapshot> snapshot() {
        std::lock_guard lock(mutex_);
        return published_;
    }
    std::uint64_t submit(Command command) {
        std::lock_guard lock(mutex_);
        if (commands_.size() >= 8) throw std::runtime_error("Wait for the current action to finish.");
        command.serial = ++serial_;
        commands_.push_back(std::move(command));
        changed_.notify_all();
        return serial_;
    }
};
void App::prepare(std::stop_token stop) {
    const auto &catalog = store_->catalog();
    const auto file = path(catalog.gamePath);
    if (!file.is_absolute() || lower(utf8(file.filename())) != "gw2-64.exe")
        throw std::runtime_error("Select your installed Gw2-64.exe in Settings.");
    if (!std::filesystem::exists(file.parent_path() / "Gw2.dat"))
        throw std::runtime_error("Finish installing GW2 first; Gw2.dat must be beside Gw2-64.exe.");
    publish();
    const auto installed = Image(file).build();
    wantedBuild_ = remoteBuild(latestBuild(stop));
    if (wantedBuild_ < 10000 || wantedBuild_ > 10000000)
        throw std::runtime_error("Invalid GW2 build number.");
    const bool newerBuild = installed < wantedBuild_;
    if (!newerBuild && !catalog.updatePending) return;
    if (running())
        throw std::runtime_error(newerBuild
                ? "A GW2 update is available. Close all game clients, then launch again."
                : "A previous GW2 download needs to finish. Close all game clients, then launch again.");
    store_->updatePending(true);
    start({}, stop);
}
} // namespace gw2

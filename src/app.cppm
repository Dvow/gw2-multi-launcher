module;
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef GW2_HELPER_MAGIC
#define GW2_HELPER_MAGIC 0x374C4D47
#endif

export module app;
import game;
import platform;
import accounts;
import app_extension;
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
    automaticUpdates,
    connect,
    guard,
    verify,
    cancelConnect,
    displayName,
    cancelSetup,
    launchGame, // The normal launch: GW2's own sign-in, without a saved account.
    installation // Saves only the game, runner, prefix, and Proton fields of `settings`.
};
// The normal launch's session key. Account IDs are UUIDs and the updater's is empty,
// so it names neither.
inline constexpr std::string_view normalSession = "gw2";
struct Command {
    Action action{};
    std::uint64_t serial{};
    std::string id{}, label{}, email{}, arguments{}, beforeId{};
    std::vector<std::string> ids{};
    std::vector<std::string> dlls{};
    std::vector<Program> programs{};
    Secret password{};
    unsigned pid{};
    Provider provider{};
    bool enable{};
    Catalog settings{};
    ProxySettings proxy{};
    Secret proxyPassword{};
    bool clearProxyPassword{};
};
struct SessionView {
    std::string id, status;
    std::string email{};
    unsigned state{}, pid{}, verification{};
    bool active{}, canShow{}, closing{};
    bool wegame{}; // Started by WeGame's bridge (helper operation 5).
    bool normal() const { return id == normalSession; }
    bool needsVerification() const { return active && !closing && (state == 12 || state == 13); }
    bool canClose() const { return active && !closing && !id.empty(); }
    // A normal launch waiting for its player to sign in has released GW2's instance lock.
    // One waiting for WeGame (15), or signing in through it (2), has a game that holds the
    // lock until its window opens: the helper reports Running once it has released it.
    bool blocking() const {
        return active && (closing || id.empty() || (state < 4 && !(normal() && !wegame && state == 2)) ||
            state == 9 || state == 10 || state == 11 || state == 15 || needsVerification());
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
    struct Error {
        std::string message;
        bool selectGame{};
    } error;
    std::string editId, editEmail;
    // When each account's game last closed (Unix seconds), for its row's "Last played".
    std::map<std::string, std::int64_t, std::less<>> played;
    std::uint64_t completed{};
    bool ready{}, busy{}, fatal{}, launchesQueued{};
    std::vector<std::string> queuedLaunches;
    bool updating() const {
        return update.stage == UpdateStage::installing || update.stage == UpdateStage::installed;
    }
    bool launchQueued(std::string_view id) const {
        return std::ranges::find(queuedLaunches, id) != queuedLaunches.end();
    }
    const SessionView *session(std::string_view id) const {
        const auto found = std::ranges::find(sessions, id, &SessionView::id);
        return found == sessions.end() ? nullptr : &*found;
    }
    std::int64_t lastPlayed(std::string_view id) const {
        const auto found = played.find(id);
        return found == played.end() ? 0 : found->second;
    }
};

class App {
    static constexpr std::uint32_t magic = GW2_HELPER_MAGIC;
    // Extension launch preparation (for example a client update) runs off the
    // worker thread. The worker copies progress text and finishes the launch.
    struct Preparation {
        std::mutex mutex;
        std::string status; // Guarded by mutex.
        std::stop_source stop;
        std::vector<std::string> dlls;
        std::string prefix;
    };
    struct Session {
        SessionView view;
        std::unique_ptr<Process> process;
        Secret outgoing;
        std::size_t written{}, received{};
        std::array<unsigned char, 12> record{};
        std::uint64_t deadline{};
        std::uint64_t started{};
        bool show{}, failed{}, closeSent{};
        bool played{}; // Reached Running: its game's close is the account's "Last played".
        bool managedDll{}; // The extension's DLL is loaded first, outside the user's list.
        std::optional<std::uint32_t> updaterExit; // GW2's updater ended with this failure code (helper state 17).
        std::shared_ptr<Preparation> preparation;
        std::future<PreparedLaunch> prepared;
    };
    // A program waiting for its delay. It starts only while one of the launches it came
    // with is queued or running, so a cancelled, failed, or exited launch starts no more.
    struct PendingProgram {
        std::string path;
        std::vector<std::string> owners;
        std::chrono::steady_clock::time_point due;
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
    std::vector<PendingProgram> programs_; // In start order.
    std::uint32_t wantedBuild_{};
    std::string updatePath_; // The installation the running updater patches.
    bool updateOlder_{}; // Its executable is behind the newest build (else an earlier download resumes).
    bool updateFirst_{}; // A launch failed on an outdated GW2: run its updater before the queue.
    std::unique_ptr<SteamConnection> auth_;
    std::optional<EpicConnection> epic_;
    Secret connectedSession_;
    std::future<AppUpdate> updateTask_;
    std::chrono::steady_clock::time_point updateDue_{std::chrono::steady_clock::time_point::max()};
    std::jthread worker_;

    // Release builds have no RTTI, so the type test rethrows the exception
    // being handled (callers are catch handlers) instead of using dynamic_cast.
    void reportError(const std::exception &error) {
        bool gamePath = false;
        if (std::current_exception()) {
            try {
                throw;
            } catch (const GamePathRequired &) {
                gamePath = true;
            } catch (...) {
            }
        }
        state_.error = {error.what(), gamePath};
    }
    void publish() {
        state_.auth.connected = !connectedSession_.bytes.empty();
        state_.sessions.clear();
        for (const auto &s : sessions_)
            state_.sessions.push_back(s.view);
        state_.launchesQueued = !launches_.empty();
        state_.queuedLaunches.assign(launches_.begin(), launches_.end());
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
    // An account's game that reached Running closed: its row shows "Last played" from now. Its
    // helper wrote the file just before reporting the close; this covers a game recalled from an
    // earlier launcher run, whose helper may predate the file.
    void recordPlayed(Session &session) {
        if (!std::exchange(session.played, false)) return;
        const auto &id = session.view.id;
        const auto &accounts = store_->catalog().accounts;
        if (std::ranges::find(accounts, id, &Account::id) == accounts.end()) return;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        state_.played[id] = now;
        try {
            savePlayed(id, now);
        } catch (const std::exception &) {
        }
    }
    // A launch WeGame's bridge starts and signs in (helper operation 5): a WeGame account, or
    // the normal launch of a WeGame install.
    // The installation a session starts: the updater's, the normal launch's, or its account's.
    const std::string &gameOf(std::string_view id) const {
        static const std::string none;
        const auto &catalog = store_->catalog();
        if (id.empty()) return updatePath_;
        if (id == normalSession) return normalGamePath(catalog);
        const auto found = std::ranges::find(catalog.accounts, id, &Account::id);
        return found == catalog.accounts.end() ? none : gamePathFor(catalog, *found);
    }
    bool viaWeGame(std::string_view id) const {
        if (id == normalSession) return wegame(normalGamePath(store_->catalog()));
        const auto &accounts = store_->catalog().accounts;
        const auto found = std::ranges::find(accounts, id, &Account::id);
        return found != accounts.end() && found->provider == Provider::wegame;
    }
    static std::string failure(std::uint32_t code, bool managedDll = false) {
        if ((code & 0xFF000000u) == 0x40000000u) {
            const auto index = (code >> 16) & 0xFF;
            if (managedDll && index == 1)
                return std::string(appExtension().managedName) + " could not be copied or loaded (Windows error " +
                    std::to_string(code & 0xFFFF) +
                    "). Check temporary-folder access and your antivirus. Close this client before retrying.";
            return "DLL " + std::to_string(index - managedDll) +
                " could not be copied or loaded (Windows error " + std::to_string(code & 0xFFFF) +
                "). Check the file, temporary-folder access, its 64-bit dependencies, and initialization. "
                "Close this client before retrying.";
        }
        switch (code) {
        case 1020:
#ifdef _WIN32
            return "Proxy setup failed. Install mitmproxy, select mitmdump.exe in Settings, and allow its Windows capture helper.";
#else
            return "Proxy setup failed. Check mitmproxy 12.2+, Python 3, bubblewrap and iproute2, and free space for this account's Wine prefix. Close any other launch of this account and retry.";
#endif
        case 1021:
            return "The account proxy rejected a connection or could not be reached. Check its address, port, credentials, and allowed destinations. The client was stopped.";
        case 1022:
            return "The proxy capture helper stopped responding. The client was stopped; retry the launch.";
        case 1023:
            return "The Windows capture helper cannot run under Wine. Reinstall the complete Linux launcher package.";
        case 1024:
            return "The isolated proxy network could not start. Check that bubblewrap can create user/network namespaces and /dev/net/tun is available. Select a working Linux mitmdump 12.2+ executable in Settings. The client was not started.";
        case 1001:
            return "Wine could not resolve the game path.";
        case 1002:
            return "Another GW2 client is blocking new ones: one opened outside this launcher, or one that "
                   "failed before signing in. Close it, then try again.";
        case 1003:
            return "GW2 needs attention. Select Show to open its window.";
        case 1004:
            return "Sign-in needs attention. Select Show; no automatic retry was sent.";
        case 1005:
            return "Close every GW2 client before updating, and check that Gw2.dat is writable.";
        case 1006:
            return "The official GW2 updater did not finish. Launch again to resume it.";
        case 1041:
        case 1042:
        case 1043: {
            // GW2 adds its Settings.json arguments to every start, the updater's too, and
            // refuses to patch with these (native/patch_flags.hpp). The China client reads
            // the same file. Not "%APPDATA%": a text pattern would read "%A" as a value.
            const std::string flags =
                code == 1041 ? "-shareArchive" : code == 1042 ? "-noPatch" : "-shareArchive and -noPatch";
#ifdef _WIN32
            return "GW2 cannot update while its Settings.json holds " + flags +
                ". Remove that from \"arguments\" in AppData\\Roaming\\Guild Wars 2\\Settings.json, then launch "
                "again. The launcher adds what it needs itself.";
#else
            return "GW2 cannot update while its Settings.json holds " + flags +
                ". Remove that from \"arguments\" in AppData/Roaming/Guild Wars 2/Settings.json inside the Wine "
                "prefix, then launch again. The launcher adds what it needs itself.";
#endif
        }
        case 1007:
            return "GW2's platform connection could not be prepared. Close the game and try again.";
        case 1008:
            return "Could not terminate this GW2 process. Try Close again or end it in Task Manager.";
        case 1030:
            return "WeGame's GW2WGLauncher.exe is missing beside Gw2-64.exe. Repair the game in WeGame.";
        case 1031:
            return "WeGame runs Guild Wars 2 as administrator. Run the launcher as administrator, then launch again.";
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
        case 1015:
            return "Could not connect to Steam. Check your internet connection, then launch again. "
                   "There is no need to sign in again.";
        default:
            if (code > 20000 && code < 85536)
                return "GW2 sign-in returned error " + std::to_string(code - 20000) +
                    ". Select Show to check its window.";
            return "The game helper reported error " + std::to_string(code) +
                ". Check its window before retrying.";
        }
    }
    // Failure 1006 with the code GW2's updater ended on: 1 is GW2's own fatal error, which it
    // shows in a window and writes to ArenaNet.log; other codes are an end from outside or a
    // crash before GW2 could report it.
    static std::string updaterFailure(std::uint32_t exitCode) {
        // Small codes in decimal, as GW2 and Task Manager give them; a Windows status
        // (0xC0000005) the way Windows writes it.
        std::string code = std::to_string(exitCode);
        if (exitCode >= 0x10000) {
            char digits[8]{};
            const auto end = std::to_chars(digits, digits + sizeof(digits), exitCode, 16).ptr;
            code.assign(digits, end);
            for (auto &digit : code)
                if (digit >= 'a') digit = static_cast<char>(digit - ('a' - 'A'));
            code.insert(0, "0x");
        }
#ifdef _WIN32
        return "The official GW2 updater ended with an error (code " + code +
            "). If GW2 showed an error window, it says why; the same text is at the end of ArenaNet.log in "
            "GW2's folder under AppData\\Roaming. Launch again to retry.";
#else
        return "The official GW2 updater ended with an error (code " + code +
            "). If GW2 showed an error window, it says why; the same text is at the end of ArenaNet.log in "
            "GW2's folder under AppData/Roaming inside the Wine prefix. Launch again to retry.";
#endif
    }
    Secret epicCredentials(const std::string &id, std::string_view name, std::stop_token stop) {
        const auto account = store_->account(id);
        auto saved = openAccount(account, stop);
        cancelled(stop);
        auto login = epicRefresh(saved, path(store_->catalog().gamePath), {});
        // Refresh tokens rotate. Commit the replacement before launching the game,
        // so cancellation or a launch failure cannot discard the saved session.
        store_->put(
            id, account.label, {}, account.arguments, account.dlls, account.programs, Secret{}, {}, Provider::epic,
            std::move(login.record));
        cancelled(stop);
        Secret request;
        tokenCredentials(request.bytes, name, login.access.text());
        return request;
    }
    void start(std::string id, std::stop_token stop) {
        const bool update = id.empty();
        if (active(id)) throw std::runtime_error("This account already has a client.");
        std::erase_if(sessions_,
            [&](const Session &existing) { return existing.view.id == id && !existing.view.active; });
        sessions_.emplace_back();
        auto &session = sessions_.back();
        session.view = {.id = id, .status = update ? "Starting updater…" : "Starting…", .active = true,
            .wegame = !update && viaWeGame(id)};
        publish();
        try {
            auto dlls = update ? std::vector<std::string>{}
                               : launchDlls(session.view.normal() ? Account{} : store_->account(id),
                                     store_->catalog());
            if (!update && appExtension().prepareLaunch) {
                auto preparation = std::make_shared<Preparation>();
                preparation->dlls = std::move(dlls);
                preparation->prefix = store_->catalog().prefix;
                session.prepared = std::async(std::launch::async,
                    [preparation, prepare = appExtension().prepareLaunch] {
                        return prepare(preparation->dlls, preparation->prefix, preparation->stop.get_token(),
                            [&](std::string status) {
                                std::lock_guard lock(preparation->mutex);
                                preparation->status = std::move(status);
                            });
                    });
                session.preparation = std::move(preparation);
                return; // poll() continues this launch when preparation finishes.
            }
            launch(session, std::move(dlls), stop);
        } catch (...) {
            sessions_.pop_back();
            publish();
            throw;
        }
        publish();
    }
    void launch(Session &session, std::vector<std::string> dlls, std::stop_token stop) {
        const auto id = session.view.id;
        const bool update = id.empty();
        // The updater and the normal launch use no saved account.
        const auto account = update || session.view.normal() ? Account{} : store_->account(id);
        // Each account starts its own client's installation: the Chinese client for KongZhong
        // and WeGame accounts (that branch has no Steam or Epic sign-in), the global one otherwise.
        const auto &game = update ? updatePath_ : gamePathFor(store_->catalog(), account);
        if (game.empty())
            throw std::runtime_error(chinaProvider(account.provider)
                    ? "KongZhong and WeGame accounts need the Chinese client. Choose its Gw2-64.exe in Settings."
                    : "This account needs Guild Wars 2. Choose its Gw2-64.exe in Settings.");
        if (account.provider == Provider::wegame && !wegame(game))
            throw std::runtime_error("WeGame accounts need the WeGame edition of the game. In Settings, choose the "
                                     "Gw2-64.exe in your WeGame game folder.");
        // WeGame starts one game at a time, and that game holds Gw2.dat for itself.
        if (!update && viaWeGame(id) && std::ranges::any_of(sessions_, [&](const Session &other) {
                return &other != &session && other.view.active && viaWeGame(other.view.id);
            }))
            throw std::runtime_error("WeGame runs one Guild Wars 2 at a time. Close the running one first.");
        Secret epicRequest;
        if (account.provider == Provider::epic) epicRequest = epicCredentials(id, "1", stop);
        const auto &catalog = store_->catalog();
        // A client started beside another running one gets its own MumbleLink.
        const bool ownMumble = std::ranges::any_of(sessions_, [&](const Session &other) {
            return &other != &session && other.view.active && !other.view.id.empty();
        });
        auto &bytes = session.outgoing.bytes;
        // Both login and proxy credentials stay in this wiping allocation: do
        // not grow the vector after appending either plaintext record.
        bytes.reserve(262144);
        if (account.provider == Provider::steam) {
            session.outgoing = steamRequest(catalog, account, openAccount(account, stop), dlls, ownMumble, stop);
#ifdef _WIN32
            constexpr auto executable = GW2_PROGRAM_FILE ".exe";
#else
            constexpr auto executable = GW2_PROGRAM_FILE;
#endif
            session.process = std::make_unique<Process>(
                std::vector<std::string>{utf8(installed(executable)), "--steam-session"},
                std::vector<std::pair<std::string, std::string>>{});
        } else {
            if (update) {
                appendNumber(bytes, magic);
                appendNumber(bytes, 1);
                wireString(bytes, game);
                appendNumber(bytes, updateOlder_ ? 1 : 0);
            } else
                gameRequest(bytes, catalog, account, dlls, ownMumble, stop);
            // The normal launch and WeGame accounts send no credentials.
            if (!account.id.empty() && account.provider != Provider::wegame) {
                auto secret =
                    account.provider == Provider::epic ? std::move(epicRequest) : openAccount(account, stop);
                if (passwordLogin(account.provider))
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
            session.process = gameRunner(catalog, game, dllDirectories(dlls), account.proxy.enabled);
        }
        session.deadline = SDL_GetTicks() + 90000;
    }
    bool pollPreparation(Session &session, std::stop_token stop) {
        auto &preparation = *session.preparation;
        if (session.view.closing || stop.stop_requested()) preparation.stop.request_stop();
        bool changed{};
        {
            std::lock_guard lock(preparation.mutex);
            if (!preparation.status.empty() && !session.view.closing) {
                session.view.status = std::exchange(preparation.status, {});
                changed = true;
            }
        }
        if (session.prepared.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return changed;
        const auto finished = std::exchange(session.preparation, {});
        try {
            auto prepared = session.prepared.get();
            session.managedDll = prepared.managedDll;
            // The game starts without the extension's DLL: say why here, as for a program that
            // failed to start. The launch itself goes on.
            if (!prepared.problem.empty() && !session.view.closing && state_.error.message.empty())
                state_.error = {std::move(prepared.problem)};
            if (!session.view.closing) {
                session.view.status = "Starting…";
                launch(session, std::move(finished->dlls), stop);
            }
        } catch (const std::exception &e) {
            if (!session.view.closing) {
                session.failed = true;
                session.view.active = false;
                session.view.status = e.what();
                session.process.reset();
                session.outgoing.clear();
                reportError(e);
                launches_.clear();
            }
        }
        if (session.view.closing) {
            session.view.active = session.view.closing = false;
            session.view.status = "Launch cancelled";
        }
        return true;
    }
    void prepare(std::stop_token stop);
    void saveAccount(Command &command, std::stop_token stop) {
        if (active(command.id)) throw std::runtime_error("Close this account's client before editing it.");
        if (std::ranges::find(launches_, command.id) != launches_.end())
            throw std::runtime_error("This account is queued to launch.");
        if (state_.auth.busy) throw std::runtime_error("Finish signing in before saving.");
        if (!connectedSession_.bytes.empty() &&
            (state_.auth.id != command.id || state_.auth.provider != command.provider))
            throw std::runtime_error("Sign in from this account's editor before saving.");
        // Saving may fail validation or disk I/O. Keep the verified
        // draft session until the catalog has actually been committed.
        Secret candidate;
        candidate.bytes.assign(connectedSession_.bytes.begin(), connectedSession_.bytes.end());
        const auto id = command.id;
        store_->putProxy(std::move(command.id), std::move(command.label), std::move(command.email),
            std::move(command.arguments), std::move(command.dlls), std::move(command.programs),
            std::move(command.password), stop,
            command.provider, std::move(candidate), std::move(command.proxy),
            std::move(command.proxyPassword), command.clearProxyPassword);
        clearAuthentication();
        if (id.empty()) return;
        const auto &account = store_->account(id);
        state_.auth = {id, {}, {}, account.identity, account.username, false, false, account.provider};
    }
    void editAccount(Command &command, std::stop_token stop) {
        if (active(command.id)) throw std::runtime_error("Close this account's client before editing it.");
        if (std::ranges::find(launches_, command.id) != launches_.end())
            throw std::runtime_error("This account is queued to launch.");
        clearAuthentication();
        const auto &account = store_->account(command.id);
        state_.editEmail.clear();
        if (passwordLogin(account.provider)) {
            auto secret = openAccount(account, stop);
            state_.editEmail = fromUtf16(std::span(secret.bytes).subspan(8, (number(secret.bytes) + 1) * 2));
        } else if (platformLogin(account.provider)) {
            (void)openAccount(account, stop);
            state_.auth = {command.id, {}, {}, account.identity, account.username};
        }
        state_.auth.provider = account.provider;
        state_.editId = command.id;
    }
    void queueLaunch(Command &command, std::stop_token stop) {
        const bool game = command.action == Action::launchGame;
        if (game) {
            // Launching GW2 again shows the running game instead of starting another.
            if (active(normalSession)) {
                for (auto &session : sessions_)
                    session.show |= session.view.normal() && session.view.active && !session.view.closing;
                return;
            }
            command.ids = {std::string(normalSession)};
        } else {
            if (command.ids.empty()) throw std::runtime_error("Select an account to launch.");
            // Resolve every requested identity before preparing any client. A stale or empty
            // selection must never fall back to launching all accounts.
            for (const auto &id : command.ids)
                (void)store_->account(id);
        }
        const bool underway = !launches_.empty() || blocked();
        std::vector<std::string> added;
        const auto queue = [&](const std::string &id) {
            if (active(id) || std::ranges::find(launches_, id) != launches_.end()) return;
            launches_.push_back(id);
            added.push_back(id);
        };
        if (game) queue(command.ids.front());
        for (const auto &a : store_->catalog().accounts)
            if (std::ranges::find(command.ids, a.id) != command.ids.end()) queue(a.id);
        if (added.empty() && launches_.empty() && !blocked())
            throw std::runtime_error("The requested accounts are already running.");
        if (added.empty()) return;
        if (!underway) {
            updateFirst_ = false;
            prepare(stop);
        }
        // Global programs, then each newly queued account's own, each after the
        // previous one's delay. Those without a delay start now.
        auto due = std::chrono::steady_clock::now();
        const auto schedule = [&](const std::vector<Program> &programs, const std::vector<std::string> &owners) {
            for (const auto &program : programs)
                programs_.push_back({program.path, owners, due += std::chrono::seconds(program.delay)});
        };
        schedule(store_->catalog().programs, added);
        for (const auto &id : added)
            if (id != normalSession) schedule(store_->account(id).programs, {id});
        pollPrograms();
    }
    // A launch still queued, or its game running and neither failed nor closing.
    bool launching(std::string_view id) const {
        return std::ranges::find(launches_, id) != launches_.end() ||
            std::ranges::any_of(sessions_, [&](const Session &s) {
                return s.view.id == id && s.view.active && !s.view.closing && !s.failed;
            });
    }
    // Starts the programs whose delay has passed and drops those whose launches ended.
    // A program that fails to start is reported; the game launch goes on.
    bool pollPrograms() {
        if (programs_.empty()) return false;
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> due;
        std::erase_if(programs_, [&](const PendingProgram &program) {
            if (std::ranges::none_of(program.owners, [&](const std::string &id) { return launching(id); }))
                return true;
            if (program.due > now) return false;
            due.push_back(program.path);
            return true;
        });
        if (due.empty()) return false;
        auto failure = startPrograms(due);
        if (failure.empty() || !state_.error.message.empty()) return false;
        state_.error = {std::move(failure)};
        return true;
    }
    void setupAccount(Command &command, std::stop_token stop) {
        if (!validDisplayName(command.label))
            throw std::runtime_error("Use 3–27 letters and single spaces for your GW2 display name.");
        auto found = std::ranges::find_if(sessions_, [&](const Session &s) {
            return s.view.id == command.id && s.view.active && !s.view.closing && s.view.state == 9;
        });
        if (found == sessions_.end() || !found->outgoing.bytes.empty() ||
            !platformLogin(store_->account(command.id).provider))
            throw std::runtime_error("This account is not waiting for a GW2 display name.");
        Secret retry;
        const bool epic = store_->account(command.id).provider == Provider::epic;
        if (epic) retry = epicCredentials(command.id, command.label, stop);
        auto &bytes = found->outgoing.bytes;
        bytes.reserve(1 + retry.bytes.size() + command.label.size() + 4);
        bytes.push_back(3);
        if (epic)
            bytes.insert(bytes.end(), retry.bytes.begin(), retry.bytes.end());
        else
            textField(bytes, command.label);
        found->view.state = 2;
        found->view.status = "Creating GW2 account…";
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
                s.view.pid == command.pid && !s.view.closing;
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
        if (!session.view.canClose()) return;
        if (state_.error.message == session.view.status) state_.error = {};
        session.view.closing = true;
        session.view.canShow = false;
        session.view.status = "Terminating…";
        session.closeSent = false;
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
        case Action::installation: {
            // Repair keeps every other setting and any account edit as it is. The chosen
            // executable replaces its own client's installation, global or Chinese.
            auto next = store_->catalog();
            auto chosen = trim(command.settings.gamePath);
            (chinaClient(chosen) ? next.chinaGamePath : next.gamePath) = std::move(chosen);
            next.runner = std::move(command.settings.runner);
            next.prefix = std::move(command.settings.prefix);
            next.proton = std::move(command.settings.proton);
            command.settings = std::move(next);
            [[fallthrough]];
        }
        case Action::settings: {
            // Refuse only a change that moves a running client or a queued launch to another
            // installation. Settings fills a newly found Chinese client by itself; refusing that
            // while global games ran failed every later Settings save until they closed.
            const auto &next = command.settings;
            const auto moves = [&](std::string_view id) {
                if (id.empty()) return next.gamePath != store_->catalog().gamePath ||
                    next.chinaGamePath != store_->catalog().chinaGamePath; // the updater
                if (id == normalSession) return normalGamePath(next) != gameOf(id);
                const auto &accounts = store_->catalog().accounts;
                const auto found = std::ranges::find(accounts, id, &Account::id);
                return found != accounts.end() && gamePathFor(next, *found) != gameOf(id);
            };
            const bool movesRunning = std::ranges::any_of(
                sessions_, [&](const Session &s) { return s.view.active && moves(s.view.id); });
            if (movesRunning || std::ranges::any_of(launches_, moves))
                throw std::runtime_error(movesRunning
                        ? "Close your clients before changing the game installation."
                        : "Wait for the current launch to finish before changing the game installation.");
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
            if (std::ranges::find(launches_, command.id) != launches_.end())
                throw std::runtime_error("This account is queued to launch.");
            store_->remove(command.id);
            if (state_.played.erase(command.id)) {
                std::error_code error;
                std::filesystem::remove(playedFile(command.id), error);
            }
            break;
        case Action::edit:
            editAccount(command, stop);
            break;
        case Action::reorder:
            store_->reorder(command.id, command.beforeId);
            break;
        case Action::launch:
        case Action::launchGame:
            queueLaunch(command, stop);
            break;
        case Action::checkUpdate:
        case Action::installUpdate:
            beginUpdate(command.action == Action::installUpdate, stop);
            break;
        case Action::automaticUpdates:
            store_->automaticUpdates(command.enable);
            if (command.enable) beginUpdate(false, stop);
            break;
        case Action::show:
            for (auto &session : sessions_)
                if (session.view.id == command.id && session.view.active && !session.view.closing)
                    session.show = true;
            break;
        case Action::close:
        case Action::closeAll:
        case Action::cancelSetup:
            if (command.action != Action::close)
                launches_.clear();
            else
                std::erase(launches_, command.id);
            for (auto &session : sessions_)
                if (command.action == Action::closeAll || (session.view.id == command.id &&
                        (!command.pid || session.view.pid == command.pid)))
                    closeSession(session);
            break;
        case Action::displayName:
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
        if (command.action != Action::close && command.action != Action::closeAll &&
            command.action != Action::cancelSetup) {
            state_.error = {};
            state_.editId.clear();
            state_.editEmail.clear();
        }
        try {
            if (command.action == Action::launch || command.action == Action::launchGame ||
                command.action == Action::edit || command.action == Action::save ||
                command.action == Action::remove || command.action == Action::settings ||
                command.action == Action::installation) {
                if (state_.updating())
                    throw std::runtime_error("Wait for the current update to finish.");
            } else if (command.action != Action::show && command.action != Action::close &&
                command.action != Action::closeAll && command.action != Action::guard &&
                command.action != Action::verify && command.action != Action::cancelConnect &&
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
            reportError(e);
            if (command.action == Action::launch || command.action == Action::launchGame) launches_.clear();
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
            reportError(e);
            state_.auth.busy = state_.auth.code = false;
            state_.auth.qr.clear();
            state_.auth.prompt.clear();
            auth_.reset();
            epic_.reset();
            connectedSession_.clear();
        }
        return true;
    }
    // A client started with -shareArchive cannot patch itself. When the asset host has a
    // newer build, its patcher asserts "Client needs to be patched first." at startup.
    bool outdated(const std::string &game, std::stop_token stop) const {
        try {
            const Image image(path(game));
            return image.build() < remoteBuild(latestBuild(image.patchHost(), stop));
        } catch (const std::exception &) {
            return false; // Unchecked, the helper's own failure stands.
        }
    }
    void receiveStatus(Session &session, std::stop_token stop) {
        session.received = 0;
        if (number(session.record) != magic)
            throw std::runtime_error(
                "The Wine/Proton runner changed the helper protocol. Use wine or umu-run directly.");
        const auto state = number(session.record, 4), detail = number(session.record, 8);
        // 7, 8 and 17 belong to the updater, which sends nothing else but its exit and failure.
        const bool updaterState = state == 7 || state == 8 || state == 17;
        if (state < 1 || state > 17 || (session.view.id.empty() ? !updaterState && state != 5 && state != 6
                                        : updaterState))
            throw std::runtime_error("Unexpected game helper status.");
        // GW2's updater ended with a failure code; the failure itself (1006) follows.
        if (state == 17) {
            session.updaterExit = detail;
            return;
        }
        if ((state == 12 || state == 13) && detail != 1 && detail != 2 && detail != 4 && detail != 5 && detail != 6)
            throw std::runtime_error("Unknown GW2 verification request. Select Show to check the game.");
        if (state == 1) {
            session.view.pid = detail;
            if (!session.view.id.empty()) session.started = launchedGame(detail).started;
            if (session.started) storeLaunches();
        }
        const bool closing = session.view.closing;
        if (closing) {
            if (state == 5)
                session.failed = false;
            else if (state == 6 && detail == 1008) {
                session.view.closing = false;
                session.closeSent = false;
            } else
                return; // Native work interrupted by Kill can report a stale login/load error.
        }
        // An add-on in the running game asked for a restart to install its update (helper state
        // 16): close the game, then launch it again ahead of the rest of the queue. That launch
        // checks for the add-on's update first (recheckLaunch).
        if (state == 16) {
            if (session.failed || session.view.state != 4) return;
            if (std::ranges::find(launches_, session.view.id) == launches_.end())
                launches_.push_front(session.view.id);
            if (appExtension().recheckLaunch) appExtension().recheckLaunch();
            closeSession(session);
            session.view.status = "Restarting to install an update…";
            return;
        }
        // A GW2 release published after the launch's build check. The assert leaves a crash
        // window (a failure) or ends the client (an exit). Close it, run GW2's updater, then
        // launch it again ahead of the rest of the queue. Not a WeGame launch: WeGame updates it.
        bool update{};
        if ((state == 5 || state == 6) && !closing && !session.failed && !session.view.id.empty() &&
            session.view.state != 4 && !viaWeGame(session.view.id) && outdated(gameOf(session.view.id), stop)) {
            launches_.push_front(session.view.id);
            updateFirst_ = update = true;
            if (state == 6) {
                closeSession(session);
                session.view.status = "Closing GW2 to install its update…";
                return;
            }
        }
        if (state == 5 || state == 8) {
            session.view.pid = 0;
            session.started = 0;
        }
        // Only this launcher's own deadline (poll) leaves a session failed that later runs:
        // a helper never reports Running after its own failure.
        if (state == 4 && session.failed) {
            if (state_.error.message == session.view.status) state_.error = {};
            session.failed = false;
        }
        if (state == 4) session.played = true;
        if (state == 5) recordPlayed(session);
        session.view.state = session.failed && state != 5 ? 6 : state;
        session.view.verification = state == 12 || state == 13 ? detail : 0;
        if (state == 3 || state == 4 || state == 5 || state == 6) session.view.email.clear();
        if (state == 6) {
            session.failed = true;
            session.view.status = detail == 1006 && session.updaterExit ? updaterFailure(*session.updaterExit)
                                                                        : failure(detail, session.managedDll);
            launches_.clear();
            state_.error = {session.view.status};
        } else if (!session.failed) {
            constexpr const char *statuses[]{"", "Starting…", "Signing in…", "Opening game…", "Running",
                "Client exited", "", "Updating GW2…", "Updated", "Choose a GW2 display name",
                "Review GW2's agreement", "Loading DLLs…", "Verification required", "Checking verification code…",
                "Preparing account proxy…", "Waiting for WeGame…"};
            session.view.status = statuses[state];
            if (state == 11)
                session.view.status = session.managedDll && detail == 1
                    ? "Loading " + std::string(appExtension().managedName) + "…"
                    : "Loading DLL " + std::to_string(detail - session.managedDll) + "…";
            if (state == 12 && detail == 5) session.view.status = "Check your email";
            if (state == 2 && viaWeGame(session.view.id))
                session.view.status = "Signing in through WeGame…";
            else if (state == 2 && session.view.normal())
                session.view.status = "Sign in through GW2";
        }
        if (state == 1 || state == 2 || state == 3 || state == 4 || state == 7 || state == 10 || state >= 11)
            session.view.canShow = true;
        if (state == 9) session.view.canShow = false;
        if (state == 14 || state == 15) session.view.canShow = false;
        if (state == 6) session.view.canShow = session.view.pid != 0;
        // A player signing in to a normal launch, or to WeGame first, may take as long as they need.
        session.deadline = (state == 2 && (session.view.normal() || viaWeGame(session.view.id))) || state == 15
            ? UINT64_MAX
            : SDL_GetTicks() + (state == 2 || state == 13 ? 130000 : 100000);
        if (state == 8) {
            const Image image(path(updatePath_));
            if (image.build() < wantedBuild_)
                throw std::runtime_error("GW2's update did not reach the current build. Try again.");
            (void)image.resolve();
            store_->updatePending({});
            // The queue's other installation may need its own update: check again first.
            if (!launches_.empty()) updateFirst_ = true;
        }
        if (state == 5 || state == 8) {
            if (state == 5 && !update && !session.failed && !closing && !launches_.empty() &&
                !session.view.normal())
                throw std::runtime_error("A client exited before all accounts finished launching.");
            session.view.active = session.view.canShow = session.view.closing = false;
            session.process.reset();
            session.outgoing.clear();
            storeLaunches();
        }
    }
    bool readStatus(Session &session, std::stop_token stop) {
        bool changed{};
        for (unsigned i = 0; i < 16 && session.view.active; ++i) {
            const auto count = session.process->read(std::span(session.record).subspan(session.received));
            if (!count) break;
            session.received += count;
            if (session.received == session.record.size()) {
                receiveStatus(session, stop);
                changed = true;
            }
        }
        return changed;
    }
    void storeLaunches() {
#ifdef _WIN32
        std::string text = "1\n";
        for (const auto &session : sessions_) {
            if (session.view.id.empty() || !session.view.active || !session.view.pid || !session.started) continue;
            text += session.view.id;
            text += ' ';
            text += std::to_string(session.view.pid);
            text += ' ';
            text += std::to_string(session.started);
            text += '\n';
        }
        try {
            atomicWrite(dataRoot() / "sessions.txt", text);
        } catch (const std::exception &) {
        }
#endif
    }
    void recallLaunches() {
#ifdef _WIN32
        std::ifstream input(dataRoot() / "sessions.txt");
        if (!input) return;
        std::string version;
        if (!std::getline(input, version) || version != "1") return;
        const auto &accounts = store_->catalog().accounts;
        for (std::string line; std::getline(input, line);) {
            const auto first = line.find(' ');
            const auto second = first == std::string::npos ? std::string::npos : line.find(' ', first + 1);
            if (first == std::string::npos || second == std::string::npos || !first || first > 64) continue;
            const auto id = line.substr(0, first);
            const auto pidText = std::string_view(line).substr(first + 1, second - first - 1);
            auto startedText = std::string_view(line).substr(second + 1);
            if (!startedText.empty() && startedText.back() == '\r') startedText.remove_suffix(1);
            unsigned pid{};
            std::uint64_t started{};
            const auto pidParsed = std::from_chars(pidText.data(), pidText.data() + pidText.size(), pid);
            const auto startedParsed =
                std::from_chars(startedText.data(), startedText.data() + startedText.size(), started);
            if (pidParsed.ec != std::errc{} || pidParsed.ptr != pidText.data() + pidText.size() ||
                startedParsed.ec != std::errc{} || startedParsed.ptr != startedText.data() + startedText.size() ||
                !pid || !started)
                continue;
            if (id != normalSession && std::ranges::find(accounts, id, &Account::id) == accounts.end()) continue;
            const auto live = launchedGame(pid);
            if (live.reachable && live.started != started) continue;
            if (std::ranges::any_of(sessions_, [&](const Session &session) {
                    return session.view.id == id || session.view.pid == pid;
                }))
                continue;
            Session session;
            session.view = {.id = id, .status = "Running", .state = 4, .pid = pid, .active = true, .canShow = true};
            session.started = started;
            session.played = true;
            sessions_.push_back(std::move(session));
        }
        storeLaunches();
#endif
    }
    bool watchLaunch(Session &session) {
        const auto live = launchedGame(session.view.pid);
        // An unreachable game (elevated, or another user's) can be neither watched nor ended:
        // its Close fails at once instead of staying "Terminating…" and holding the queue.
        if (!live.reachable && !(session.view.closing && !session.closeSent)) return false;
        if (live.reachable && live.started != session.started) {
            recordPlayed(session);
            session.view.pid = 0;
            session.started = 0;
            session.view.state = 5;
            session.view.status = "Client exited";
            session.view.active = session.view.canShow = session.view.closing = false;
            session.view.email.clear();
            storeLaunches();
            return true;
        }
        if (session.view.closing && !session.closeSent) {
            if (live.reachable && endLaunchedGame(session.view.pid, session.started)) {
                session.closeSent = true;
                return true;
            }
            session.view.closing = false;
            session.closeSent = false;
            session.view.state = 6;
            session.view.status = failure(1008);
            state_.error = {session.view.status};
            return true;
        }
        if (!session.show) return false;
        showLaunchedGame(session.view.pid, session.started);
        session.show = false;
        return true;
    }
    bool poll(Session &session, std::stop_token stop) {
        if (session.preparation) return pollPreparation(session, stop);
        if (!session.process) return watchLaunch(session);
        bool changed{};
        try {
            // Observe an already-exited client before writing another control byte.
            changed = readStatus(session, stop);
            if (!session.view.active) return true;
            if (!session.outgoing.bytes.empty()) {
                session.process->write(session.outgoing, session.written);
            } else if (session.view.closing && !session.closeSent) {
                // Finish any partial frame first; the helper also accepts Kill
                // before its process handle has been published.
                const unsigned char byte = 5;
                if (session.process->write({&byte, 1}) == 1) session.closeSent = true;
            } else if (session.show) {
                const unsigned char byte = 1;
                session.process->allowForeground();
                if (session.process->write({&byte, 1}) == 1) session.show = false;
            }
            if (!session.view.closing &&
                (session.view.state < 4 || session.view.state == 11 || session.view.state == 13) &&
                SDL_GetTicks() > session.deadline) {
                // A login timeout does not prove the game exited. Retain its
                // helper so Show and Kill still work after this failure.
                session.failed = true;
                session.view.state = 6;
                session.view.canShow = true;
                session.view.status = "The game helper timed out before opening the game. Select Show or Close.";
                state_.error = {session.view.status};
                launches_.clear();
                changed = true;
            }
            cancelled(stop);
        } catch (const std::exception &e) {
            // A natural exit can close stdin between the read and Kill write.
            // Its queued exit record still wins over that broken-pipe error.
            if (session.view.closing) {
                try {
                    readStatus(session, stop);
                    if (!session.view.active) return true;
                } catch (...) {
                }
            }
            if (!session.failed || session.view.closing) {
                session.view.status = session.view.closing
                    ? "Could not confirm that GW2 exited. " + std::string(e.what()) : e.what();
                if (session.view.closing)
                    state_.error = {session.view.status};
                else
                    reportError(e);
            }
            session.failed = true;
            session.view.active = session.view.canShow = session.view.closing = false;
            session.view.pid = 0;
            session.started = 0;
            session.view.email.clear();
            session.process.reset();
            session.outgoing.clear();
            storeLaunches();
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
            reportError(e);
        }
        return true;
    }
    void beginUpdate(bool install, std::stop_token stop) {
        if (!appUpdatesEnabled() || updateTask_.valid()) return;
        if (install && (state_.auth.busy || !launches_.empty()))
            throw std::runtime_error(state_.auth.busy
                    ? "Finish signing in before updating the launcher."
                    : "Wait for the current launch to finish before updating the launcher.");
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
        const bool quiet = state_.update.stage != UpdateStage::checking &&
            state_.update.stage != UpdateStage::installing;
        try {
            state_.update = updateTask_.get();
        } catch (const std::exception &e) {
            if (state_.update.stage == UpdateStage::checking || state_.update.stage == UpdateStage::installing)
                state_.update.stage = state_.update.version.empty() ? UpdateStage::idle : UpdateStage::available;
            if (!quiet) state_.update.error = e.what();
        }
        if (state_.update.stage != UpdateStage::installing && state_.update.stage != UpdateStage::installed)
            updateDue_ = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        return true;
    }
    void run(std::stop_token stop) {
        try {
            store_ = std::make_unique<Store>();
            recallLaunches();
            // Also the closes that helpers recorded while the launcher was closed.
            for (const auto &account : store_->catalog().accounts)
                if (const auto time = lastPlayed(account.id)) state_.played[account.id] = time;
            if (appExtension().start) appExtension().start(stop);
            state_.ready = true;
            if (store_->catalog().autoUpdate) beginUpdate(false, stop);
            publish();
        } catch (const std::exception &e) {
            reportError(e);
            state_.fatal = true;
            publish();
            if (appExtension().stop) appExtension().stop();
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
            if (appUpdatesEnabled() && store_ && store_->catalog().autoUpdate &&
                std::chrono::steady_clock::now() >= updateDue_ && !updateTask_.valid() &&
                state_.update.stage != UpdateStage::checking &&
                state_.update.stage != UpdateStage::installing &&
                state_.update.stage != UpdateStage::installed) {
                updateTask_ = std::async(std::launch::async, [stop] { return checkAppUpdate(stop); });
                updateDue_ = std::chrono::steady_clock::now() + std::chrono::minutes(5);
            }
            change |= pollAuthentication(stop);
            for (auto &session : sessions_)
                if (session.view.active) change |= poll(session, stop);
            if (!blocked() && !launches_.empty() && !stop.stop_requested()) {
                try {
                    if (std::exchange(updateFirst_, false))
                        prepare(stop); // Its updater blocks the queue until GW2 is current.
                    else {
                        auto id = std::move(launches_.front());
                        launches_.pop_front();
                        start(std::move(id), stop);
                    }
                } catch (const std::exception &e) {
                    reportError(e);
                    launches_.clear();
                    change = true;
                }
            }
            change |= pollPrograms();
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
        // Destroying a session waits for its preparation; ask it to stop first.
        for (auto &session : sessions_)
            if (session.preparation) session.preparation->stop.request_stop();
        sessions_.clear();
        if (appExtension().stop) appExtension().stop();
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
        const auto limit = command.action == Action::launch ? 100u : 8u;
        if (commands_.size() >= limit) throw std::runtime_error("Wait for the current action to finish.");
        command.serial = ++serial_;
        commands_.push_back(std::move(command));
        changed_.notify_all();
        return serial_;
    }
};
// The installations the queued launches start, each checked once, then the first that needs
// GW2's updater gets it; the queue checks again after each update (receiveStatus).
void App::prepare(std::stop_token stop) {
    const auto &catalog = store_->catalog();
    struct Needed {
        std::string game;
        bool china{}, wegameOnly{true};
    };
    std::vector<Needed> games;
    for (const auto &id : launches_) {
        const auto &game = gameOf(id);
        auto found = std::ranges::find(games, game, &Needed::game);
        if (found == games.end()) {
            const auto account = std::ranges::find(catalog.accounts, id, &Account::id);
            games.push_back({game,
                account == catalog.accounts.end() ? catalog.gamePath.empty() : chinaProvider(account->provider),
                true});
            found = games.end() - 1;
        }
        found->wegameOnly &= viaWeGame(id);
    }
    for (const auto &needed : games) {
        auto single = catalog;
        single.gamePath = needed.game;
        single.chinaGamePath.clear();
        const auto problem = needed.game.empty()
            ? std::string(needed.china ? "Choose the Chinese client's Gw2-64.exe in Settings." : "Choose your Gw2-64.exe.")
            : installationProblem(single);
        if (problem.empty()) continue;
        // The home page repairs the normal launch's installation; the other is in Settings.
        if (needed.game == normalGamePath(catalog)) throw GamePathRequired{problem};
        throw std::runtime_error(problem);
    }
    publish();
    for (const auto &needed : games) {
        // WeGame updates its install before its bridge starts the game: WeGame launches alone
        // run no updater here, which would also move the install past WeGame's own build.
        if (needed.wegameOnly) continue;
        const auto &game = needed.game;
        const Image image(path(game));
        const auto wanted = remoteBuild(latestBuild(image.patchHost(), stop));
        if (wanted < 10000 || wanted > 10000000) throw std::runtime_error("Invalid GW2 build number.");
        const bool newerBuild = image.build() < wanted;
        if (!newerBuild && store_->catalog().updatePending != game) continue;
        if (std::ranges::any_of(sessions_, [&](const Session &s) { return s.view.active && gameOf(s.view.id) == game; }))
            throw std::runtime_error(newerBuild
                    ? "A GW2 update is available. Close this installation's game clients, then launch again."
                    : "A previous GW2 download needs to finish. Close this installation's game clients, then launch again.");
        wantedBuild_ = wanted;
        updatePath_ = game;
        updateOlder_ = newerBuild;
        store_->updatePending(game);
        start({}, stop);
        return;
    }
}
} // namespace gw2

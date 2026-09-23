#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <mutex>
#include <utility>

import game;

extern "C" __declspec(dllimport) DWORD __cdecl KxExecute(
    HWND, DWORD, DWORD, const wchar_t *, const wchar_t *, DWORD *, DWORD *, DWORD *, const gw2::Layout *);

namespace {
constexpr DWORD Magic = 0x36585747; // Protocol 6: launch settings, credentials and verification/control messages.
struct Failure {
    DWORD code;
};
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};
struct Secrets {
    std::vector<BYTE> bytes;
    Secrets() = default;
    Secrets(const Secrets &) = delete;
    Secrets &operator=(const Secrets &) = delete;
    Secrets(Secrets &&other) noexcept : bytes(std::move(other.bytes)) {}
    Secrets &operator=(Secrets &&other) noexcept {
        SecureZeroMemory(bytes.data(), bytes.size());
        bytes = std::move(other.bytes);
        return *this;
    }
    ~Secrets() { SecureZeroMemory(bytes.data(), bytes.size()); }
    const wchar_t *name() const { return reinterpret_cast<const wchar_t *>(bytes.data() + 8); }
    const wchar_t *password() const { return name() + wcslen(name()) + 1; }
};
struct Client {
    DWORD pid{};
    HWND login{}, game{}, dialog{};
    bool hide{};
    gw2::Layout layout{};
};

void Read(void *data, DWORD size) {
    auto p = static_cast<BYTE *>(data);
    while (size) {
        DWORD n{};
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), p, size, &n, nullptr) || !n) throw Failure{1000};
        p += n;
        size -= n;
    }
}
DWORD Number() {
    DWORD n{};
    Read(&n, sizeof(n));
    return n;
}
Secrets ReadCredentials(DWORD operation, bool initial = false) {
    Secrets secret;
    const auto size = Number();
    if (size < 14 || size > (operation == 3 ? 16524u : operation == 2 ? 1850u : 1162u)) throw Failure{1000};
    secret.bytes.resize(size);
    Read(secret.bytes.data(), size);
    DWORD lengths[2]{};
    memcpy(lengths, secret.bytes.data(), 8);
    if (!lengths[0] || lengths[0] >= (operation >= 2 ? 64u : 320u) || !lengths[1] ||
        lengths[1] >= (operation == 3          ? 8192u
                              : operation == 2 ? 600u
                                               : 256u) ||
        size != 8 + (lengths[0] + lengths[1] + 2) * 2)
        throw Failure{1000};
    const auto name = secret.name(), password = name + lengths[0] + 1;
    if (name[lengths[0]] || password[lengths[1]] || wcslen(name) != lengths[0] ||
        wcslen(password) != lengths[1])
        throw Failure{1000};
    if (operation >= 2) {
        if (initial) {
            if (lengths[0] != 1 || name[0] != L'1') throw Failure{1000};
        } else {
            if (lengths[0] < 3 || lengths[0] > 27 || name[0] == L' ' || name[lengths[0] - 1] == L' ')
                throw Failure{1000};
            for (DWORD i = 0; i < lengths[0]; ++i)
                if (!((name[i] >= L'A' && name[i] <= L'Z') || (name[i] >= L'a' && name[i] <= L'z') ||
                        (name[i] == L' ' && (!i || name[i - 1] != L' '))))
                    throw Failure{1000};
        }
    }
    if (operation == 2) {
        if (lengths[1] % 2) throw Failure{1000};
        for (DWORD i = 0; i < lengths[1]; ++i)
            if (!(password[i] >= L'0' && password[i] <= L'9') &&
                !(password[i] >= L'a' && password[i] <= L'f'))
                throw Failure{1000};
    }
    return secret;
}
std::wstring String() {
    const auto size = Number();
    if (size > 32760) throw Failure{1000};
    std::wstring value(size, L'\0');
    Read(value.data(), size * 2);
    if (value.find(L'\0') != std::wstring::npos) throw Failure{1000};
    return value;
}
void Report(DWORD state, DWORD detail = 0) {
    DWORD record[]{Magic, state, detail}, written{};
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), record, sizeof(record), &written, nullptr) ||
        written != sizeof(record))
        throw Failure{1000};
}
std::wstring DosPath(const std::wstring &path) {
    if (path.empty() || path[0] != L'/') return path;
    using Convert = WCHAR *(__cdecl *)(const char *);
    auto convert = reinterpret_cast<Convert>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
    if (!convert) throw Failure{1001};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    auto value = convert(utf8.c_str());
    if (!value) throw Failure{1001};
    std::wstring result(value);
    HeapFree(GetProcessHeap(), 0, value);
    return result;
}
std::wstring Quote(const std::wstring &value) {
    std::wstring result = L"\"";
    size_t slashes{};
    for (const auto ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (ch == L'"') result += L'\\';
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
BOOL CALLBACK Window(HWND window, LPARAM parameter) {
    auto &client = *reinterpret_cast<Client *>(parameter);
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != client.pid) return TRUE;
    wchar_t name[128]{};
    GetClassNameW(window, name, 128);
    if (!wcscmp(name, L"ArenaNet") && !GetWindow(window, GW_OWNER))
        client.login = window;
    else if (!wcscmp(name, L"ArenaNet_Gr_Window_Class"))
        client.game = window;
    else if (IsWindowVisible(window) &&
        (!wcscmp(name, L"#32770") || (!wcscmp(name, L"ArenaNet") && GetWindow(window, GW_OWNER))))
        client.dialog = window;
    return TRUE;
}
void Observe(Client &client) {
    client.login = client.game = client.dialog = nullptr;
    EnumWindows(Window, reinterpret_cast<LPARAM>(&client));
}
void Reveal(Client &client, bool focus = false) {
    Observe(client);
    for (const auto window : {client.login, client.game, client.dialog})
        if (window) ShowWindowAsync(window, SW_SHOWNA);
    auto target = client.dialog ? client.dialog : client.game ? client.game : client.login;
    if (focus && target) {
        ShowWindowAsync(target, IsIconic(target) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(target);
    }
}
// Wine imports Unix pipes as character handles: PeekNamedPipe cannot poll them.
// This process-lifetime reader owns stdin after the request. Its mailbox also
// lives until process exit; teardown never destroys a mutex under a blocked reader.
std::atomic<unsigned> control{1}; // Connected, Show requested, invalid command.
struct RegistrationInput {
    std::mutex mutex;
    Secrets request, code;
};
RegistrationInput &registration = *new RegistrationInput;
Secrets TakeCredentials() {
    std::lock_guard lock(registration.mutex);
    return std::move(registration.request);
}
Secrets TakeCode() {
    std::lock_guard lock(registration.mutex);
    return std::move(registration.code);
}
DWORD WINAPI ReadControl(void *parameter) {
    const auto operation = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(parameter));
    bool initial = true;
    BYTE command{};
    DWORD count{};
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), &command, 1, &count, nullptr) && count) {
        if (command == 2) break;
        if (command == 5) {
            control.fetch_or(16);
            continue;
        }
        if (command == 4) {
            control.fetch_or(8);
            continue;
        }
        if (command == 6) {
            try {
                const auto size = Number();
                if (size != 12 && size != 14) throw Failure{1000};
                Secrets code;
                code.bytes.resize(size);
                Read(code.bytes.data(), size);
                const auto text = reinterpret_cast<const wchar_t *>(code.bytes.data());
                if (text[size / 2 - 1]) throw Failure{1000};
                for (DWORD i = 0; i < size / 2 - 1; ++i)
                    if (text[i] < L'0' || text[i] > L'9') throw Failure{1000};
                std::lock_guard lock(registration.mutex);
                if (!registration.code.bytes.empty()) throw Failure{1000};
                registration.code = std::move(code);
                continue;
            } catch (...) {
                control.fetch_or(4);
                break;
            }
        }
        if (command == 3) {
            try {
                auto request = ReadCredentials(operation, initial);
                std::lock_guard lock(registration.mutex);
                if (!registration.request.bytes.empty()) throw Failure{1000};
                registration.request = std::move(request);
                initial = false;
                continue;
            } catch (...) {
                control.fetch_or(4);
                break;
            }
        }
        if (command != 1) {
            control.fetch_or(4);
            break;
        }
        control.fetch_or(2);
    }
    control.fetch_and(~1u);
    return 0;
}
void StartControl(DWORD operation = 0) {
    Handle thread{CreateThread(
        nullptr, 0, ReadControl, reinterpret_cast<void *>(static_cast<ULONG_PTR>(operation)), 0, nullptr)};
    if (!thread.value) throw Failure{GetLastError()};
}
bool Control(Client &client) {
    const auto state = control.fetch_and(~18u);
    if (state & 4) throw Failure{1000};
    if (!(state & 1)) return false;
    if (state & 2) {
        client.hide = false;
        Reveal(client, true);
    }
    if (state & 16) {
        Observe(client);
        const auto window = client.game ? client.game : client.login;
        if (window && !PostMessageW(window, WM_CLOSE, 0, 0)) throw Failure{1008};
    }
    return true;
}
DWORD Native(Client &client, DWORD operation, DWORD &flags, const wchar_t *email = nullptr,
    const wchar_t *password = nullptr) {
    DWORD result{}, error{};
    const auto transport = KxExecute(
        client.login, client.pid, operation, email, password, &result, &flags, &error, &client.layout);
    if (transport) throw Failure{transport};
    if (result == 6) throw Failure{error};
    if (result == 9) throw Failure{20000 + error};
    if (result == 3 || result == 5 || result == 0) throw Failure{1100 + result};
    return result;
}

void Update(const std::wstring &executable, const std::wstring &folder, Client &client) {
    // Wine starts its persistent desktop lazily. Start it outside the updater
    // job so shell services cannot keep that job alive after patching finishes.
    if (!GetDesktopWindow()) throw Failure{1006};
    // The official patcher owns archive writes. Refuse if any client has the
    // archive open; a job tracks self-updater children without killing on close.
    {
        Handle archive{CreateFileW((folder + L"\\Gw2.dat").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (archive.value == INVALID_HANDLE_VALUE) throw Failure{1005};
    }
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job.value) throw Failure{GetLastError()};
    auto command = Quote(executable) + L" -image -dat " + Quote(folder + L"\\Gw2.dat");
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, folder.c_str(), &startup, &info))
        throw Failure{GetLastError()};
    Handle process{info.hProcess}, thread{info.hThread};
    client.pid = info.dwProcessId;
    if (!AssignProcessToJobObject(job.value, process.value) ||
        ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        const auto error = GetLastError();
        TerminateProcess(process.value, error);
        throw Failure{error};
    }
    Report(7);
    for (;;) {
        if (!Control(client)) {
            Reveal(client);
            return;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(
                job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr))
            throw Failure{GetLastError()};
        if (!accounting.ActiveProcesses) break;
        Sleep(100);
    }
    DWORD exitCode{};
    if (!GetExitCodeProcess(process.value, &exitCode) || exitCode) throw Failure{1006};
    Report(8);
}
struct Launch {
    enum Phase { starting = 1, signingIn, launching, running, registration = 9, cancelling, agreement,
        verification, checkingCode };
    Client &client;
    HANDLE process;
    DWORD operation;
    Secrets secret;
    std::vector<std::wstring> dlls;
    Phase phase{starting};
    DWORD flags{};
    ULONGLONG deadline{GetTickCount64() + 90000};
    void signIn() {
        if (Native(client, 3, flags) != 1) throw Failure{1004};
        if (Native(client,
                operation == 3       ? 7u
                    : operation == 2 ? 5u
                                     : 1u,
                flags, secret.name(), secret.password()) != 1)
            throw Failure{1004};
        SecureZeroMemory(secret.bytes.data(), secret.bytes.size());
        Report(2);
        phase = signingIn;
        deadline = GetTickCount64() + 120000;
    }
    void setup() {
        if (control.fetch_and(~8u) & 8) {
            if (!client.login || !PostMessageW(client.login, WM_CLOSE, 0, 0)) throw Failure{1004};
            phase = cancelling;
            deadline = GetTickCount64() + 10000;
        }
        auto retry = TakeCredentials();
        if (!retry.bytes.empty() && phase == registration) {
            if (operation < 2 || !retry.name()[0] ||
                Native(client, operation == 3 ? 7u : 5u, flags, retry.name(), retry.password()) != 1)
                throw Failure{1004};
            Report(2);
            phase = signingIn;
            deadline = GetTickCount64() + 120000;
        }
    }
    void signedIn() {
        if (client.game) {
            Reveal(client);
            Report(4);
            phase = running;
            return;
        }
        if (!client.login) throw Failure{1004};
        const auto result = Native(client, 4, flags);
        if (result == 12) {
            if (phase != verification) Report(12, (flags >> 24) & 7);
            phase = verification;
        } else if (result == 8 && operation >= 2) {
            Report(9);
            phase = registration;
        } else if (result == 10) {
            client.hide = false;
            Reveal(client, true);
            Report(10);
            phase = agreement;
        } else if (result == 7) {
            Report(3);
            phase = launching;
            deadline = GetTickCount64() + 90000;
        } else if (result != 1 || !(flags & (2 | 4)))
            throw Failure{1004};
    }
    void verify() {
        auto code = TakeCode();
        if (code.bytes.empty() || phase != verification) return;
        const auto type = (flags >> 24) & 7;
        const auto result = Native(client, 9, flags, nullptr,
            reinterpret_cast<const wchar_t *>(code.bytes.data()));
        if (result != 1 && result != 4) throw Failure{1004};
        Report(13, type);
        phase = checkingCode;
        deadline = GetTickCount64() + 120000;
    }
    void observe() {
        // If native acceptance needs attention, reveal its prompt and observe
        // continuation without repeatedly accepting or invoking Play.
        if (phase == agreement && !client.game && (!client.login || Native(client, 6, flags) == 7)) {
            Report(3);
            phase = launching;
            deadline = GetTickCount64() + 90000;
        }
        if ((phase == launching || phase == agreement) && client.game) {
            Reveal(client);
            Report(4);
            phase = running;
        }
        if ((phase < running || phase == cancelling || phase == checkingCode) && GetTickCount64() > deadline)
            throw Failure{WAIT_TIMEOUT};
    }
    void run() {
        while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            if (!Control(client)) {
                Reveal(client);
                return;
            }
            Observe(client);
            if (phase != running && (client.dialog || (client.login && !IsWindowEnabled(client.login))))
                throw Failure{1003};
            if (client.hide && (phase < running || phase == registration || phase == verification ||
                    phase == checkingCode) && client.login &&
                IsWindowVisible(client.login))
                ShowWindowAsync(client.login, SW_HIDE);
            // Loading GW2 and fetching a platform token overlap. Only validated
            // credentials from the pipe reader may cross into native sign-in.
            if (phase == starting && secret.bytes.empty()) secret = TakeCredentials();
            if (phase == starting && client.login && Native(client, 0, flags) == 1) {
                for (std::size_t i = 0; i < dlls.size(); ++i) {
                    if (!Control(client)) {
                        Reveal(client);
                        return;
                    }
                    Report(11, static_cast<DWORD>(i + 1));
                    try {
                        if (Native(client, 8, flags, nullptr, dlls[i].c_str()) != 1)
                            throw Failure{ERROR_DLL_INIT_FAILED};
                    } catch (const Failure &failure) {
                        // Preserve the launch-list index and loader error in one
                        // existing status record. Never retry an uncertain load.
                        throw Failure{0x40000000u | (static_cast<DWORD>(i + 1) << 16) |
                            (failure.code > 0xFFFF ? 0xFFFF : failure.code)};
                    }
                }
                if (!dlls.empty()) deadline = GetTickCount64() + 90000;
                dlls.clear();
                if (!Control(client)) {
                    Reveal(client);
                    return;
                }
                if (!secret.bytes.empty()) signIn();
            }
            if (phase == registration) setup();
            if (phase == verification) verify();
            if (phase == signingIn || phase == verification || phase == checkingCode) signedIn();
            observe();
            Sleep(phase == running ? 100 : 16);
        }
        Report(5);
    }
};
void OpenInput(Handle &input, const char *path) {
    using Convert = WCHAR *(__cdecl *)(const char *);
    auto convert = reinterpret_cast<Convert>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
    auto name = convert ? convert(path) : nullptr;
    if (!name) throw Failure{1001};
    input.value = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto error = GetLastError();
    HeapFree(GetProcessHeap(), 0, name);
    if (input.value == INVALID_HANDLE_VALUE) throw Failure{error};
    if (!SetStdHandle(STD_INPUT_HANDLE, input.value)) throw Failure{GetLastError()};
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "--input"))) return 2;
    Client client;
    Handle process, input;
    try {
        if (argc == 3) OpenInput(input, argv[2]);
        if (Number() != Magic) throw Failure{1000};
        const auto operation = Number();
        if (operation > 3) throw Failure{1000};
        const auto executable = DosPath(String());
        const auto folder = executable.substr(0, executable.find_last_of(L"\\/"));
        if (operation == 1) {
            StartControl();
            Update(executable, folder, client);
            return 0;
        }
        try {
            client.layout =
                gw2::Image(std::filesystem::path(executable)).resolve(operation >= 2 ? operation - 1 : 0);
        } catch (...) {
            throw Failure{1103};
        }
        std::wstring command = Quote(executable) + L" -dat " + Quote(folder + L"\\Gw2.dat");
        const auto count = Number();
        if (count > 256) throw Failure{1000};
        for (DWORD i = 0; i < count; ++i)
            command += L" " + Quote(String());
        if (command.size() > 32760) throw Failure{1000};
        client.hide = Number() != 0;
        const auto dllCount = Number();
        if (dllCount > 16) throw Failure{1000};
        std::vector<std::wstring> dlls;
        for (DWORD i = 0; i < dllCount; ++i) {
            auto dll = DosPath(String());
            if (dll.empty() || dll.size() >= 4096 || !std::filesystem::path(dll).is_absolute() ||
                _wcsicmp(std::filesystem::path(dll).extension().c_str(), L".dll"))
                throw Failure{1000};
            dlls.push_back(std::move(dll));
        }
        Handle existing{OpenMutexW(SYNCHRONIZE, FALSE, L"AN-Mutex-Window-Guild Wars 2")};
        if (existing.value) throw Failure{1002};
        if (GetLastError() != ERROR_FILE_NOT_FOUND) throw Failure{GetLastError()};
        StartControl(operation);
        if (!Control(client)) return 0;
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = client.hide ? SW_HIDE : SW_SHOWNORMAL;
        PROCESS_INFORMATION info{};
        // No pipe handles inherited by GW2: only this helper can read account secrets.
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                folder.c_str(), &startup, &info))
            throw Failure{GetLastError()};
        process.value = info.hProcess;
        CloseHandle(info.hThread);
        client.pid = info.dwProcessId;
        Report(1, client.pid); // Starting: detail identifies the game, not this helper.
        Launch{client, process.value, operation, {}, std::move(dlls)}.run();
    } catch (const Failure &failure) {
        client.hide = false;
        Reveal(client);
        try {
            Report(6, failure.code);
            // Preserve Show and process observation after a challenge or uncertain result.
            while (process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
                if (!Control(client)) return 0;
                Sleep(100);
            }
            Report(5);
        } catch (...) {
            Reveal(client);
        }
        return 1;
    } catch (...) {
        Reveal(client);
        try {
            Report(6, 1000);
        } catch (...) {
        }
        return 1;
    }
    return 0;
}

#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <sddl.h>
#include <bcrypt.h>
#include <cwchar>
#include <cstring>
#include <initializer_list>
#include <MinHook.h>

import game;

namespace {
struct HandleEntry {
    HANDLE handle;
    ULONG_PTR handleCount, pointerCount;
    ULONG grantedAccess, objectType, attributes, reserved;
};
struct Snapshot {
    ULONG_PTR count, reserved;
    HandleEntry entries[1];
};
using QueryProcess = LONG(NTAPI *)(HANDLE, ULONG, void *, ULONG, ULONG *);
using CompareObjects = LONG(NTAPI *)(HANDLE, HANDLE);
using QuerySystem = LONG(NTAPI *)(ULONG, void *, ULONG, ULONG *);
struct SystemEntry {
    void *object;
    ULONG_PTR pid, handle;
    ULONG access;
    USHORT trace, type;
    ULONG attributes, reserved;
};
struct SystemSnapshot {
    ULONG_PTR count, reserved;
    SystemEntry entries[1];
};
DWORD ReleaseProcessMutex(
    const Snapshot &snapshot, ULONG capacity, HANDLE reference, CompareObjects compare) {
    if (snapshot.count > (capacity - 2 * sizeof(ULONG_PTR)) / sizeof(HandleEntry)) return ERROR_BAD_LENGTH;
    ULONG type{}, closed{};
    for (ULONG_PTR i = 0; i < snapshot.count; ++i)
        if (snapshot.entries[i].handle == reference) type = snapshot.entries[i].objectType;
    if (!type) return ERROR_INVALID_HANDLE;
    for (ULONG_PTR i = 0; i < snapshot.count; ++i) {
        const auto &entry = snapshot.entries[i];
        if (entry.handle == reference || entry.objectType != type || compare(reference, entry.handle) != 0)
            continue;
        if (!CloseHandle(entry.handle)) return GetLastError();
        ++closed;
    }
    return closed ? ERROR_SUCCESS : ERROR_NOT_FOUND;
}
DWORD ReleaseWineMutex(
    HMODULE ntdll, void *buffer, ULONG capacity, HANDLE reference, CompareObjects compare) {
    // Wine lacks ProcessHandleInformation. Its system query includes other
    // processes: filter ownership before comparing, and never use Object pointers.
    auto query = reinterpret_cast<QuerySystem>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    auto snapshot = static_cast<SystemSnapshot *>(buffer);
    ULONG returned{}, closed{};
    if (!GetProcAddress(ntdll, "wine_get_version") || !query ||
        query(64, snapshot, capacity, &returned) < 0 ||
        snapshot->count > (capacity - 2 * sizeof(ULONG_PTR)) / sizeof(SystemEntry))
        return ERROR_BAD_LENGTH;
    for (ULONG_PTR i = 0; i < snapshot->count; ++i) {
        const auto &entry = snapshot->entries[i];
        const auto handle = reinterpret_cast<HANDLE>(entry.handle);
        if (entry.pid != GetCurrentProcessId() || handle == reference || compare(reference, handle) != 0)
            continue;
        if (!CloseHandle(handle)) return GetLastError();
        ++closed;
    }
    return closed ? ERROR_SUCCESS : ERROR_NOT_FOUND;
}
} // namespace

// Only runs inside the client that received our scoped native request. Compare
// kernel object identity against the exact mutex before closing its leaked handle.
DWORD ReleaseInstanceMutex() {
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = reinterpret_cast<QueryProcess>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    auto compare = reinterpret_cast<CompareObjects>(GetProcAddress(ntdll, "NtCompareObjects"));
    if (!query || !compare) return ERROR_PROC_NOT_FOUND;
    auto reference = OpenMutexW(SYNCHRONIZE, FALSE, L"AN-Mutex-Window-Guild Wars 2");
    if (!reference) {
        auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    constexpr ULONG capacity = 4 * 1024 * 1024;
    auto buffer = static_cast<Snapshot *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity));
    if (!buffer) {
        CloseHandle(reference);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    ULONG returned{};
    const auto status = query(GetCurrentProcess(), 51, buffer, capacity, &returned);
    const auto error = status < 0 ? ReleaseWineMutex(ntdll, buffer, capacity, reference, compare)
                                  : ReleaseProcessMutex(*buffer, capacity, reference, compare);
    HeapFree(GetProcessHeap(), 0, buffer);
    CloseHandle(reference);
    return error;
}

namespace {
constexpr wchar_t MessageName[] = L"KX.GW2MultiLauncher.Native.9";
constexpr DWORD Magic = 0x31584B47;

enum Operation : DWORD {
    Probe = 0,
    Login = 1,
    Play = 2,
    EnableMultiple = 3,
    PlayIfAuthenticated = 4,
    SteamLogin = 5,
    ObservePlay = 6,
    EpicLogin = 7
};
enum Result : DWORD {
    Pending = 0,
    Completed = 1,
    InvalidTarget = 2,
    UnsupportedBuild = 3,
    Busy = 4,
    InvalidRequest = 5,
    NativeFault = 6,
    PlayRequested = 7,
    NameRequired = 8,
    AuthenticationError = 9,
    AgreementRequired = 10
};

// One fixed value packet. No host-process pointers are meaningful to the hook.
struct Packet {
    DWORD magic, version, targetPid, operation;
    DWORD result, flags, exceptionCode, reserved;
    std::uint64_t nonce;
    wchar_t email[320];
    wchar_t password[8192];
    gw2::Layout layout;
};
static_assert(sizeof(Packet) == ((17064 + sizeof(gw2::Layout) + 7) & ~std::size_t{7}));
HMODULE moduleHandle{};

struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
};
struct View {
    Packet *value{};
    ~View() {
        if (value) UnmapViewOfFile(value);
    }
};
struct Hook {
    HHOOK value{};
    ~Hook() {
        if (value) UnhookWindowsHookEx(value);
    }
};
struct LocalBuffer {
    void *value{};
    ~LocalBuffer() {
        if (value) LocalFree(value);
    }
};

void MappingName(wchar_t (&name)[96], DWORD caller, std::uint64_t nonce) {
    swprintf_s(name, L"Local\\KX.GW2MultiLauncher.%lu.%016llX", caller, nonce);
}

bool IsReadable(const void *address, SIZE_T size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    auto start = reinterpret_cast<std::uintptr_t>(address);
    auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start <= end && size <= end - start;
}

// A native notification subscription outlives an individual window-hook call.
// Pin this observer's module for the client lifetime, so GW2 never retains a
// callback into an unloaded DLL. Only the launcher's owner thread reads/writes it.
struct LoginObserver {
    const std::uintptr_t *table{};
    DWORD thread{}, error{};
    void *context{};
    void(__fastcall *unsubscribe)(void *, void *){};
    HWND window{};
} loginObserver;
std::uintptr_t loginNotifications[17]{};
struct ChannelHook {
    gw2::Anchor original{}, installed{};
    unsigned provider{};
} channelHook;

unsigned __fastcall PlatformChannel() {
    return channelHook.provider;
}

bool EnablePlatformChannel(std::uintptr_t base, const gw2::Anchor &anchor, unsigned provider) {
    if (channelHook.original.rva) return channelHook.provider == provider;
    if (MH_Initialize() != MH_OK) return false;
    const auto target = reinterpret_cast<void *>(base + anchor.rva);
    channelHook.provider = provider;
    if (MH_CreateHook(target, reinterpret_cast<void *>(&PlatformChannel), nullptr) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        MH_Uninitialize();
        return false;
    }
    // The pinned bridge owns this hook until game exit. Keep both instruction
    // snapshots so later requests reject foreign hooks instead of skipping validation.
    channelHook.original = channelHook.installed = anchor;
    std::memcpy(channelHook.installed.bytes.data(), target, anchor.size);
    return true;
}

void __fastcall LoginError(void *self, const unsigned short *error, unsigned, unsigned) {
    if (self != &loginObserver || GetCurrentThreadId() != loginObserver.thread) return;
    __try {
        if (IsReadable(error, sizeof(*error))) loginObserver.error = *error;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        loginObserver.error = 0;
    }
}

LRESULT CALLBACK LoginWindow(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR);
void DetachLoginObserver() {
    if (!loginObserver.context || loginObserver.thread != GetCurrentThreadId()) return;
    // The login thread removes its own listener before the context's TagList dies.
    // Pinning the DLL only keeps callbacks valid; it does not release subscriptions.
    loginObserver.unsubscribe(loginObserver.context, &loginObserver);
    RemoveWindowSubclass(loginObserver.window, LoginWindow, 1);
    loginObserver = {};
}
LRESULT CALLBACK LoginWindow(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        __try {
            DetachLoginObserver();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Never let a foreign native failure cross the window callback.
        }
        RemoveWindowSubclass(window, LoginWindow, 1);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

struct NativeRequest {
    Packet *request;
    const gw2::Layout &layout{request->layout};
    const std::uintptr_t base{reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))};
    std::uintptr_t *context{}, *table{};
    std::uintptr_t launcher{};
    DWORD flags{};
    bool connected{};
    DWORD validate(HWND window) {
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        if (!IsReadable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            dos->e_lfanew > 0x1000)
            return UnsupportedBuild;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        if (!IsReadable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.TimeDateStamp != layout.timestamp ||
            nt->OptionalHeader.SizeOfImage != layout.imageSize)
            return UnsupportedBuild;
        if (channelHook.original.rva && !layout.tokenLogin) return InvalidRequest;
        for (const auto &anchor : layout.anchors) {
            if (&anchor >= &layout.anchors[21] && !layout.anchors[21].rva) continue;
            if (((&anchor >= &layout.anchors[8] && &anchor <= &layout.anchors[14]) ||
                    &anchor == &layout.anchors[16] || &anchor == &layout.anchors[20]) &&
                !layout.tokenLogin)
                continue;
            const auto *expected = &anchor;
            if (&anchor == &layout.anchors[16] && channelHook.original.rva) {
                if (std::memcmp(&anchor, &channelHook.original, sizeof(anchor))) return UnsupportedBuild;
                expected = &channelHook.installed;
            }
            if (!anchor.rva || !anchor.size || anchor.size > anchor.bytes.size() ||
                anchor.rva >= layout.imageSize || anchor.size > layout.imageSize - anchor.rva ||
                !IsReadable(reinterpret_cast<const void *>(base + anchor.rva), anchor.size) ||
                std::memcmp(
                    reinterpret_cast<const void *>(base + anchor.rva), expected->bytes.data(), anchor.size))
                return UnsupportedBuild;
        }
        for (const auto rva : {layout.context, layout.contextTable, layout.login, layout.launcher,
                 layout.launcherTable, layout.connection, layout.play})
            if (!rva || rva >= layout.imageSize || layout.imageSize - rva < 256) return UnsupportedBuild;
        context = reinterpret_cast<std::uintptr_t *>(base + layout.context);
        if (!IsReadable(context, 24) || context[0] != base + layout.contextTable) return UnsupportedBuild;
        table = reinterpret_cast<std::uintptr_t *>(context[0]);
        if (!IsReadable(table, 32 * sizeof(void *)) || table[0] != base + layout.login ||
            (layout.tokenLogin && table[2] != base + layout.tokenLogin))
            return UnsupportedBuild;
        if (!IsReadable(reinterpret_cast<void *>(base + layout.launcher), sizeof(void *)) ||
            !IsReadable(reinterpret_cast<void *>(base + layout.connection), sizeof(DWORD)))
            return UnsupportedBuild;
        launcher = *reinterpret_cast<std::uintptr_t *>(base + layout.launcher);
        if (!IsReadable(reinterpret_cast<void *>(launcher), 176) ||
            *reinterpret_cast<std::uintptr_t *>(launcher) != base + layout.launcherTable ||
            *reinterpret_cast<HWND *>(launcher + 64) != window)
            return InvalidTarget;
        flags = *reinterpret_cast<const DWORD *>(reinterpret_cast<const BYTE *>(context) + 16);
        connected = *reinterpret_cast<const DWORD *>(base + layout.connection) == 1;
        request->flags = flags | (connected ? 0x80000000u : 0);
        if ((flags & 0x80) == 0 || !*reinterpret_cast<void **>(launcher + 160) ||
            (*reinterpret_cast<const DWORD *>(launcher + 40) & 0x2000) == 0)
            return Busy;
        return Completed;
    }
    DWORD play() {
        if (layout.tokenLogin && loginObserver.thread == GetCurrentThreadId() && loginObserver.error &&
            !connected && !(flags & (2 | 4))) {
            request->exceptionCode = loginObserver.error;
            return loginObserver.error == 3061 ? NameRequired : AuthenticationError;
        }
        if (!connected || (flags & (2 | 4)) != 0)
            return request->operation == PlayIfAuthenticated ? Completed : Busy;
        DetachLoginObserver();
        const auto starting = [&] { return (*reinterpret_cast<const DWORD *>(launcher + 40) & 0x100) != 0; };
        if (starting()) return PlayRequested;
        if (request->operation == ObservePlay) return AgreementRequired;
        if (table[13] != base + layout.anchors[17].rva || table[15] != base + layout.anchors[18].rva)
            return UnsupportedBuild;
        // Record the current agreement through GW2's normal owner-thread action.
        // Play rechecks acceptance; a rejected action still reveals the native prompt.
        using HasAgreement = bool(__fastcall *)(void *);
        using AcceptAgreement = void(__fastcall *)(void *);
        if (!reinterpret_cast<HasAgreement>(table[15])(context))
            reinterpret_cast<AcceptAgreement>(table[13])(context);
        using NativePlay = void(__fastcall *)(void *);
        reinterpret_cast<NativePlay>(base + layout.play)(reinterpret_cast<void *>(launcher));
        return starting() ? PlayRequested : AgreementRequired;
    }
    DWORD subscribe(HWND window) {
        if (table[16] != base + layout.anchors[12].rva || table[17] != base + layout.anchors[20].rva)
            return UnsupportedBuild;
        if (!loginObserver.thread) {
            HMODULE retained{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<const wchar_t *>(&LoginError), &retained)) {
                request->exceptionCode = GetLastError();
                return NativeFault;
            }
            for (auto &notification : loginNotifications)
                notification = base + layout.anchors[14].rva;
            loginNotifications[11] = reinterpret_cast<std::uintptr_t>(&LoginError);
            loginObserver.table = loginNotifications;
            loginObserver.thread = GetCurrentThreadId();
            using Subscribe = void(__fastcall *)(void *, void *);
            loginObserver.context = context;
            loginObserver.unsubscribe = reinterpret_cast<Subscribe>(table[17]);
            loginObserver.window = window;
            if (!SetWindowSubclass(window, LoginWindow, 1, 0)) {
                loginObserver = {};
                return NativeFault;
            }
            reinterpret_cast<Subscribe>(table[16])(context, &loginObserver);
        }
        if (loginObserver.thread != GetCurrentThreadId()) return InvalidTarget;
        return Completed;
    }
    DWORD platformLogin(HWND window) {
        const auto result = subscribe(window);
        if (result != Completed) return result;
        auto preferences = reinterpret_cast<std::uintptr_t *>(base + layout.preferences);
        if (!IsReadable(preferences, sizeof(void *))) return UnsupportedBuild;
        const auto prefsTable = *preferences;
        constexpr auto tableSize = 80 * sizeof(void *);
        if (prefsTable < base || prefsTable - base >= layout.imageSize ||
            layout.imageSize - (prefsTable - base) < tableSize ||
            !IsReadable(reinterpret_cast<void *>(prefsTable), tableSize))
            return UnsupportedBuild;
        auto getters = reinterpret_cast<const std::uintptr_t *>(prefsTable);
        for (const auto slot : {18, 22, 79}) {
            MEMORY_BASIC_INFORMATION info{};
            const auto function = getters[slot];
            if (function < base || function - base >= layout.imageSize ||
                !VirtualQuery(reinterpret_cast<void *>(function), &info, sizeof(info)) ||
                info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                !(info.Protect &
                    (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                return UnsupportedBuild;
        }
        // CLI providers also start the store SDK's authentication. Submit
        // our selected session with an explicit provider, using the same
        // signing-in notification and preferences as the validated token wrapper.
        using SigningIn = void(__fastcall *)(void *);
        using Number = unsigned(__fastcall *)(void *);
        using Text = const char *(__fastcall *)(void *);
        using Submit = void(__fastcall *)(const wchar_t *, const wchar_t *, unsigned, unsigned, const char *,
            const wchar_t *, unsigned, unsigned);
        // The separate game handshake must identify the same store as the token.
        // Change only that provider query; account verification remains server-owned.
        const bool epic = request->operation == EpicLogin;
        if (!EnablePlatformChannel(base, layout.anchors[16], epic ? 2 : 1)) {
            request->exceptionCode = 1007;
            return NativeFault;
        }
        loginObserver.error = 0;
        reinterpret_cast<SigningIn>(base + layout.anchors[7].rva)(context);
        const auto value22 = reinterpret_cast<Number>(getters[22])(preferences);
        const auto value18 = reinterpret_cast<Text>(getters[18])(preferences);
        const auto value79 = reinterpret_cast<Number>(getters[79])(preferences);
        if (epic) {
            char token[8192]{};
            const auto size = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, request->password, -1, token, sizeof(token), nullptr, nullptr);
            if (!size) return InvalidRequest;
            using SetToken = void(__fastcall *)(const char *);
            reinterpret_cast<SetToken>(base + layout.anchors[21].rva)(token);
            SecureZeroMemory(token, sizeof(token));
        }
        reinterpret_cast<Submit>(base + layout.tokenSubmit)(request->email,
            epic ? nullptr : request->password, 3, value79, value18, epic ? L"Epic" : L"Steam", value22, 0);
        return Completed;
    }
    DWORD login(HWND window) {
        if (request->operation != Login && request->operation != SteamLogin &&
            request->operation != EpicLogin)
            return InvalidRequest;
        const bool token = request->operation != Login;
        const bool epic = request->operation == EpicLogin;
        if (epic != (layout.anchors[21].rva != 0)) return UnsupportedBuild;
        if (token &&
            (!layout.tokenLogin || layout.anchors[8].rva != layout.tokenLogin ||
                layout.anchors[9].rva != layout.tokenLogin + 128 || !layout.tokenSubmit ||
                layout.anchors[11].rva != layout.tokenSubmit || !layout.preferences ||
                layout.preferences >= layout.imageSize || layout.imageSize - layout.preferences < 8))
            return UnsupportedBuild;
        if ((!token && !request->email[0]) || !request->password[0] || !wmemchr(request->email, 0, 320) ||
            !wmemchr(request->password, 0,
                epic        ? 8192
                    : token ? 600
                            : 256))
            return InvalidRequest;
        if (connected || (flags & (2 | 4 | 0x100)) != 0) return Busy;

        if (token) {
            const auto result = platformLogin(window);
            if (result != Completed) return result;
        } else {
            using NativeLogin = void(__fastcall *)(void *, const wchar_t *, const wchar_t *, unsigned);
            reinterpret_cast<NativeLogin>(table[0])(context, request->email, request->password, 3);
        }
        request->flags = *reinterpret_cast<const DWORD *>(reinterpret_cast<const BYTE *>(context) + 16);
        return Completed; // Submission acknowledged; this does not mean authenticated.
    }
    DWORD apply(HWND window) {
        const auto result = validate(window);
        if (result != Completed) return result;
        switch (request->operation) {
        case Probe:
            return Completed;
        case EnableMultiple:
            request->exceptionCode = ReleaseInstanceMutex();
            return request->exceptionCode ? NativeFault : Completed;
        case Play:
        case PlayIfAuthenticated:
        case ObservePlay:
            return play();
        default:
            return login(window);
        }
    }
};
DWORD Apply(Packet *request, HWND window) {
    return NativeRequest{request}.apply(window);
}

DWORD ApplyGuarded(Packet *request, HWND window) {
    __try {
        return Apply(request, window);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        request->exceptionCode = GetExceptionCode();
        return NativeFault;
    }
}

bool CurrentUserSecurity(LocalBuffer &descriptor) {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return false;
    alignas(TOKEN_USER) BYTE buffer[1024]{};
    DWORD needed{};
    if (!GetTokenInformation(token.value, TokenUser, buffer, sizeof(buffer), &needed)) return false;
    LocalBuffer sid;
    if (!ConvertSidToStringSidW(
            reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid, reinterpret_cast<wchar_t **>(&sid.value)))
        return false;
    wchar_t sddl[512]{};
    swprintf_s(sddl, L"D:P(A;;GA;;;%s)", static_cast<const wchar_t *>(sid.value));
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(
               sddl, SDDL_REVISION_1, &descriptor.value, nullptr) != FALSE;
}
} // namespace

void ReceiveRequest(const CWPSTRUCT &message) {
    if (message.message != RegisterWindowMessageW(MessageName)) return;
    wchar_t name[96]{};
    const auto nonce = static_cast<std::uint64_t>(message.lParam);
    MappingName(name, static_cast<DWORD>(message.wParam), nonce);
    Handle mapping(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name));
    if (!mapping.value) return;
    View view{static_cast<Packet *>(
        MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Packet)))};
    auto p = view.value;
    if (!p || p->magic != Magic || p->version != 9 || p->nonce != nonce ||
        p->targetPid != GetCurrentProcessId() || p->result != Pending)
        return;
    const auto result = ApplyGuarded(p, message.hwnd);
    SecureZeroMemory(p->email, sizeof(p->email));
    SecureZeroMemory(p->password, sizeof(p->password));
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&p->result), result);
}
extern "C" __declspec(dllexport) LRESULT CALLBACK KxLauncherHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) ReceiveRequest(*reinterpret_cast<const CWPSTRUCT *>(lParam));
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

extern "C" __declspec(dllexport) DWORD __cdecl KxExecute(HWND window, DWORD targetPid, DWORD operation,
    const wchar_t *email, const wchar_t *password, DWORD *result, DWORD *flags, DWORD *exceptionCode,
    const gw2::Layout *layout) {
    if (!result || !flags || !exceptionCode || !layout) return ERROR_INVALID_PARAMETER;
    *result = Pending;
    *flags = 0;
    *exceptionCode = 0;
    DWORD actualPid{};
    const auto thread = GetWindowThreadProcessId(window, &actualPid);
    if (!thread || actualPid != targetPid || operation > EpicLogin) return ERROR_INVALID_PARAMETER;
    const size_t maximum = operation == EpicLogin ? 8192 : operation == SteamLogin ? 600 : 256;
    if ((operation == Login || operation == SteamLogin || operation == EpicLogin) &&
        (!email || !password || wcsnlen_s(email, 320) >= 320 || wcsnlen_s(password, maximum) >= maximum))
        return ERROR_INVALID_PARAMETER;
    LocalBuffer descriptor;
    if (!CurrentUserSecurity(descriptor)) return GetLastError();
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor.value, FALSE};
    std::uint64_t nonce{};
    if (BCryptGenRandom(
            nullptr, reinterpret_cast<PUCHAR>(&nonce), sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return ERROR_GEN_FAILURE;
    wchar_t name[96]{};
    MappingName(name, GetCurrentProcessId(), nonce);
    Handle mapping(
        CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Packet), name));
    if (!mapping.value) return GetLastError();
    if (GetLastError() == ERROR_ALREADY_EXISTS) return ERROR_ALREADY_EXISTS;
    View view{static_cast<Packet *>(
        MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Packet)))};
    if (!view.value) return GetLastError();
    auto p = view.value;
    *p = {};
    p->magic = Magic;
    p->version = 9;
    p->targetPid = targetPid;
    p->layout = *layout;
    p->nonce = nonce;
    p->operation = operation;
    if (operation == Login || operation == SteamLogin || operation == EpicLogin) {
        wcscpy_s(p->email, email);
        wcscpy_s(p->password, password);
    }
    Hook hook{SetWindowsHookExW(WH_CALLWNDPROC, KxLauncherHook, moduleHandle, thread)};
    DWORD error{};
    if (!hook.value)
        error = GetLastError();
    else {
        DWORD_PTR ignored{};
        SetLastError(ERROR_SUCCESS);
        if (!SendMessageTimeoutW(window, RegisterWindowMessageW(MessageName), GetCurrentProcessId(),
                static_cast<LPARAM>(nonce), SMTO_ABORTIFHUNG | SMTO_BLOCK, 10000, &ignored)) {
            error = GetLastError();
            if (!error) error = ERROR_TIMEOUT;
        }
    }
    // The hook maps its own view. On timeout it retains its mapping independently
    // until the callback returns. Never unmap or free target-process pointers here.
    if (!error) {
        *result = p->result;
        *flags = p->flags;
        *exceptionCode = p->exceptionCode;
        SecureZeroMemory(p->email, sizeof(p->email));
        SecureZeroMemory(p->password, sizeof(p->password));
    } else if (!hook.value) {
        SecureZeroMemory(p->email, sizeof(p->email));
        SecureZeroMemory(p->password, sizeof(p->password));
    }
    return error;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        moduleHandle = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

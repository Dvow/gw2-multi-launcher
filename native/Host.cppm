module;
#include "proxy_runtime.hpp"
#include "process_exit.hpp"
#include "patch_flags.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <mutex>
#include <cstring>
#include <optional>
#include <utility>

#ifndef GW2_HELPER_MAGIC
#define GW2_HELPER_MAGIC 0x374C4D47
#endif
#ifndef GW2_NATIVE_CHANNEL_W
#define GW2_NATIVE_CHANNEL_W L"GW2MultiLauncher"
#endif
#ifndef GW2_NATIVE_EXECUTE
#define GW2_NATIVE_EXECUTE Gw2MultiLauncherExecute
#endif

export module native_host;
import game;

extern "C" __declspec(dllimport) DWORD __cdecl GW2_NATIVE_EXECUTE(
    HWND, DWORD, DWORD, const wchar_t *, const wchar_t *, DWORD *, DWORD *, DWORD *, const gw2::Layout *);

namespace {
constexpr DWORD Magic = GW2_HELPER_MAGIC; // Close terminates the owned game process.
// Bits: 1 connected, 2 Show, 4 invalid input, 8 Kill, 16 kill failed, 32 stop reader.
std::atomic<unsigned> control{1};
// Borrowed from main: publish before Starting, join the reader before closing
// the handle. Kill must not wait for native login or DLL loading to return.
std::atomic<HANDLE> gameProcess{};
void KillGame() {
    const auto process = gameProcess.load();
    // Either publication or the reader consumes a queued Kill, exactly once.
    // A pre-start request stays pending until creation publishes its handle.
    if (!process || !(control.fetch_and(~8u) & 8)) return;
    if (TerminateProcess(process, ERROR_PROCESS_ABORTED))
        gw2_exit::hasten(GetProcessId(process));
    else if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
        control.fetch_or(16);
}
struct Failure {
    DWORD code;
};
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};
class DllCopies {
    struct Paths {
        std::wstring folder, file;
        // A staged copy shared by every launch of the same bytes: kept after the game exits.
        bool shared{};
    };
    const Handle &process_;
    std::vector<Paths> paths_;
    // Shared copies this game loads, open with writes and deletes denied until it exits.
    std::vector<HANDLE> held_;

    void cancelled() const {
        if (!(control.load() & 1)) throw Failure{ERROR_CANCELLED};
        if (WaitForSingleObject(process_.value, 0) == WAIT_OBJECT_0) throw Failure{ERROR_PROCESS_ABORTED};
    }
    // SHA-256 of a whole file as lowercase hex; empty when it cannot be read.
    std::wstring sha256(HANDLE file) const {
        struct Hash {
            BCRYPT_ALG_HANDLE algorithm{};
            BCRYPT_HASH_HANDLE hash{};
            ~Hash() {
                if (hash) BCryptDestroyHash(hash);
                if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            }
        } state;
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN) ||
            BCryptOpenAlgorithmProvider(&state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(state.algorithm, &state.hash, nullptr, 0, nullptr, 0, 0) < 0)
            return {};
        std::vector<BYTE> bytes(1 << 20);
        for (;;) {
            cancelled();
            DWORD read{};
            if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return {};
            if (!read) break;
            if (BCryptHashData(state.hash, bytes.data(), read, 0) < 0) return {};
        }
        std::array<BYTE, 32> digest{};
        if (BCryptFinishHash(state.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return {};
        std::wstring text;
        for (const auto byte : digest) {
            text.push_back(L"0123456789abcdef"[byte >> 4]);
            text.push_back(L"0123456789abcdef"[byte & 15]);
        }
        return text;
    }
    // Writes the source, from its start, to a new file; false leaves no file behind.
    bool write(HANDLE input, const std::wstring &file) const {
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(input, start, nullptr, FILE_BEGIN)) return false;
        // Closes before deleting, also when cancelled() throws part-way.
        struct Output {
            const std::wstring &file;
            HANDLE value{INVALID_HANDLE_VALUE};
            bool keep{};
            ~Output() {
                if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
                if (!keep) DeleteFileW(file.c_str());
            }
        } output{file, CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE) {
            output.keep = true; // Not ours.
            return false;
        }
        std::array<BYTE, 65536> bytes{};
        for (;;) {
            cancelled();
            DWORD read{};
            if (!ReadFile(input, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return false;
            if (!read) break;
            for (DWORD offset = 0; offset < read;) {
                DWORD written{};
                if (!WriteFile(output.value, bytes.data() + offset, read - offset, &written, nullptr) || !written)
                    return false;
                offset += written;
            }
        }
        output.keep = CloseHandle(std::exchange(output.value, INVALID_HANDLE_VALUE)) != FALSE;
        return output.keep;
    }
    // Deletes this source's shared copies of older bytes that no game has loaded.
    static void removeOldShared(
        const std::filesystem::path &temp, const std::wstring &prefix, const std::wstring &current) noexcept {
        try {
            std::error_code error;
            for (const auto &entry : std::filesystem::directory_iterator(temp, error)) {
                const auto name = entry.path().filename().native();
                std::error_code kind;
                if (!name.starts_with(prefix) || name == current || !entry.is_directory(kind)) continue;
                std::error_code listed;
                for (const auto &file : std::filesystem::directory_iterator(entry.path(), listed))
                    DeleteFileW(file.path().c_str()); // Fails while a game has it loaded or held.
                RemoveDirectoryW(entry.path().c_str());
            }
        } catch (...) {
        }
    }
    // A new DLL file is scanned in full by antivirus when GW2 maps it: 1.5 s for a 54 MB
    // DLL on every launch. So each source's bytes are staged once, under
    // "<channel>-dll-<source path hash>-<SHA-256>", and later launches map that same file
    // again; a rebuilt source replaces its older copy once no game has that loaded. Every
    // launch checks the staged file's hash against the source and keeps it open with writes
    // and deletes denied until its game exits, so the bytes GW2 loads are this source's.
    // nullptr: use a private copy instead.
    const std::wstring *shared(HANDLE input, const std::wstring &source, const std::filesystem::path &temp,
        const std::filesystem::path &filename) {
        const auto digest = sha256(input);
        if (digest.empty()) return nullptr;
        std::uint64_t pathHash = 14695981039346656037ull; // FNV-1a of the case-folded path: names only.
        for (const auto c : source) {
            pathHash ^= static_cast<std::uint64_t>(towupper(c));
            pathHash *= 1099511628211ull;
        }
        wchar_t prefix[64]{};
        swprintf_s(prefix, GW2_NATIVE_CHANNEL_W L"-dll-%016llx-", pathHash);
        const auto folderName = std::wstring(prefix) + digest.substr(0, 32);
        const auto folder = temp / folderName;
        const auto file = (folder / filename).native();
        if (file.size() >= 4096) return nullptr;
        removeOldShared(temp, prefix, folderName);
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const auto held = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (held != INVALID_HANDLE_VALUE) {
                bool same{};
                try {
                    same = sha256(held) == digest;
                } catch (...) {
                    CloseHandle(held);
                    throw;
                }
                if (same) {
                    held_.push_back(held);
                    paths_.push_back({folder.native(), file, true});
                    return &paths_.back().file;
                }
                CloseHandle(held);
                // Damaged or replaced: write it again unless a running game has it loaded.
                if (!DeleteFileW(file.c_str())) return nullptr;
            } else if (const auto error = GetLastError(); error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                return nullptr;
            if (attempt) return nullptr;
            if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                return nullptr;
            std::uint64_t nonce{};
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce), sizeof(nonce),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                return nullptr;
            wchar_t suffix[32]{};
            swprintf_s(suffix, L".%016llx.part", nonce);
            const auto part = file + suffix;
            if (!write(input, part)) return nullptr;
            // Another launch may have placed the same bytes first; the next pass checks either.
            if (!MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH)) DeleteFileW(part.c_str());
        }
        return nullptr;
    }

  public:
    explicit DllCopies(const Handle &process) : process_(process) {}
    DllCopies(const DllCopies &) = delete;
    DllCopies &operator=(const DllCopies &) = delete;
    ~DllCopies() {
        if (paths_.empty()) return;
        // A timed-out loader can still be using a path, and the UI can close
        // before GW2. Only the actual game process exiting permits deletion.
        // This helper waits without keeping the desktop UI or secrets alive.
        if (process_.value && WaitForSingleObject(process_.value, INFINITE) != WAIT_OBJECT_0) return;
        for (const auto held : held_)
            CloseHandle(held);
        for (const auto &path : paths_) {
            if (path.shared) continue; // The next launch of the same bytes maps it again.
            DeleteFileW(path.file.c_str());
            RemoveDirectoryW(path.folder.c_str());
        }
    }
    // Copies a helper left behind when it could not wait for its game (the PC shut down, the
    // helper was ended): "<channel>-<helper pid>-<nonce>" folders whose helper no longer runs.
    // A copy a running game still has loaded cannot be deleted and stays.
    static void removeStale(const std::filesystem::path &temp) noexcept {
        try {
            const std::wstring prefix = GW2_NATIVE_CHANNEL_W L"-";
            std::error_code error;
            for (const auto &entry : std::filesystem::directory_iterator(temp, error)) {
                const auto name = entry.path().filename().native();
                std::error_code kind;
                if (!name.starts_with(prefix) || !entry.is_directory(kind)) continue;
                const auto rest = name.substr(prefix.size());
                const auto dash = rest.find(L'-');
                if (!dash || dash == rest.npos || rest.size() - dash - 1 != 16 ||
                    rest.find_first_not_of(L"0123456789") != dash ||
                    rest.find_first_not_of(L"0123456789abcdef", dash + 1) != rest.npos)
                    continue;
                const auto pid = static_cast<DWORD>(std::wcstoul(rest.c_str(), nullptr, 10));
                if (pid == GetCurrentProcessId()) continue;
                if (const auto helper = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
                    const bool running = WaitForSingleObject(helper, 0) == WAIT_TIMEOUT;
                    CloseHandle(helper);
                    if (running) continue; // That helper deletes its copies itself.
                } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
                    continue;
                }
                std::error_code listed;
                for (const auto &file : std::filesystem::directory_iterator(entry.path(), listed))
                    DeleteFileW(file.path().c_str());
                RemoveDirectoryW(entry.path().c_str());
            }
        } catch (...) {
        }
    }
    const std::wstring &copy(const std::wstring &source) {
        // Deny source writes only while taking the snapshot. Never map the
        // build output into GW2 or fall back to it when the copy fails.
        Handle input{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (input.value == INVALID_HANDLE_VALUE) throw Failure{GetLastError()};
        struct PendingCopy {
            Paths paths;
            bool folderOwned{}, fileOwned{};
            ~PendingCopy() {
                if (fileOwned) DeleteFileW(paths.file.c_str());
                if (folderOwned) RemoveDirectoryW(paths.folder.c_str());
            }
        } pending;
        Handle output;
        std::wstring temp(32768, L'\0');
        const auto length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        if (!length) throw Failure{GetLastError()};
        if (length >= temp.size()) throw Failure{ERROR_FILENAME_EXCED_RANGE};
        temp.resize(length);
        if (paths_.empty()) removeStale(temp);
        const auto filename = std::filesystem::path(source).filename();
        if (const auto staged = shared(input.value, source, temp, filename)) return *staged;
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(input.value, start, nullptr, FILE_BEGIN)) throw Failure{GetLastError()};
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            std::uint64_t nonce{};
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce), sizeof(nonce),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                throw Failure{ERROR_GEN_FAILURE};
            wchar_t name[64]{};
            swprintf_s(name, GW2_NATIVE_CHANNEL_W L"-%lu-%016llx", GetCurrentProcessId(), nonce);
            const auto folder = std::filesystem::path(temp) / name;
            pending.paths.folder = folder.native();
            pending.paths.file = (folder / filename).native();
            if (pending.paths.file.size() >= 4096) throw Failure{ERROR_FILENAME_EXCED_RANGE};
            if (CreateDirectoryW(pending.paths.folder.c_str(), nullptr)) {
                pending.folderOwned = true;
                break;
            }
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) throw Failure{error};
        }
        if (!pending.folderOwned) throw Failure{ERROR_FILE_EXISTS};
        output.value = CreateFileW(pending.paths.file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (output.value == INVALID_HANDLE_VALUE) throw Failure{GetLastError()};
        pending.fileOwned = true;
        std::array<BYTE, 65536> bytes{};
        for (;;) {
            if (!(control.load() & 1)) throw Failure{ERROR_CANCELLED};
            if (WaitForSingleObject(process_.value, 0) == WAIT_OBJECT_0)
                throw Failure{ERROR_PROCESS_ABORTED};
            DWORD read{};
            if (!ReadFile(input.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
                throw Failure{GetLastError()};
            if (!read) break;
            for (DWORD offset = 0; offset < read;) {
                DWORD written{};
                if (!WriteFile(output.value, bytes.data() + offset, read - offset, &written, nullptr))
                    throw Failure{GetLastError()};
                if (!written) throw Failure{ERROR_WRITE_FAULT};
                offset += written;
            }
        }
        // Keep the basename: add-ons may resolve their own module by name.
        // The source handle closes here; only the independent copy is loaded.
        paths_.push_back(pending.paths);
        pending.fileOwned = pending.folderOwned = false;
        return paths_.back().file;
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
        if (control.load() & 32) throw Failure{ERROR_CANCELLED};
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
// The launcher's file for when this account's game last closed (its row's "Last played"), or
// empty for the normal launch. It holds Unix seconds. A game that reached Running (4) writes it
// when it closes, before the exit report, so the launcher can read the new time. If the launcher
// closed first, this helper still waits for its game (PlayedOnExit), so that close counts too.
std::wstring playedFile;
bool reachedGame{}, playedWritten{};
void RecordPlayed() {
    if (playedFile.empty() || !reachedGame || std::exchange(playedWritten, true)) return;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto stamp = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    char text[32]{};
    const auto size = static_cast<DWORD>(
        std::snprintf(text, sizeof(text), "%llu\n", (stamp - 116444736000000000ull) / 10000000ull));
    wchar_t suffix[32]{};
    swprintf_s(suffix, L".%lu.tmp", GetCurrentProcessId());
    const auto part = playedFile + suffix;
    bool ok{};
    {
        Handle file{CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        DWORD written{};
        ok = file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, text, size, &written, nullptr) &&
            written == size;
    }
    // The launcher may replace it at the same moment with the same time: either copy is right.
    if (!ok || !MoveFileExW(part.c_str(), playedFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(part.c_str());
}
void Report(DWORD state, DWORD detail = 0) {
    if (state == 4) reachedGame = true;
    if (state == 5) RecordPlayed();
    DWORD record[]{Magic, state, detail}, written{};
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), record, sizeof(record), &written, nullptr) ||
        written != sizeof(record))
        throw Failure{1000};
}
bool WineHost() {
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}
#ifdef GW2_RUNTIME_FOLDER_W
bool SameFile(const std::filesystem::path &left, const std::filesystem::path &right) {
    std::error_code ec;
    const auto leftSize = std::filesystem::file_size(left, ec);
    if (ec) return false;
    const auto rightSize = std::filesystem::file_size(right, ec);
    if (ec || leftSize != rightSize) return false;
    HANDLE leftFile = CreateFileW(left.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE rightFile = CreateFileW(right.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (leftFile == INVALID_HANDLE_VALUE || rightFile == INVALID_HANDLE_VALUE) {
        if (leftFile != INVALID_HANDLE_VALUE) CloseHandle(leftFile);
        if (rightFile != INVALID_HANDLE_VALUE) CloseHandle(rightFile);
        return false;
    }
    std::array<unsigned char, 65536> leftBytes{}, rightBytes{};
    bool same = true;
    for (;;) {
        DWORD leftRead = 0, rightRead = 0;
        if (!ReadFile(leftFile, leftBytes.data(), static_cast<DWORD>(leftBytes.size()), &leftRead, nullptr) ||
            !ReadFile(rightFile, rightBytes.data(), static_cast<DWORD>(rightBytes.size()), &rightRead, nullptr) ||
            leftRead != rightRead || std::memcmp(leftBytes.data(), rightBytes.data(), leftRead) != 0) {
            same = false;
            break;
        }
        if (leftRead == 0) break;
    }
    CloseHandle(leftFile);
    CloseHandle(rightFile);
    return same;
}
// Under Wine, DLLs shipped in net\ beside this helper go to %APPDATA%\<runtime folder>\net in
// the game's prefix. Only a build that names a runtime folder (GW2_RUNTIME_FOLDER) ships any.
void InstallNetworkingFiles() {
    if (!WineHost()) return;
    wchar_t self[MAX_PATH]{};
    const auto selfLength = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!selfLength || selfLength >= MAX_PATH) return;
    const auto source = std::filesystem::path(self).parent_path() / L"net";
    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec)) return;
    wchar_t appdata[MAX_PATH]{};
    const auto appdataLength = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (!appdataLength || appdataLength >= MAX_PATH) throw Failure{ERROR_ENVVAR_NOT_FOUND};
    const auto destination = std::filesystem::path(appdata) / GW2_RUNTIME_FOLDER_W / L"net";
    std::filesystem::create_directories(destination, ec);
    if (ec) throw Failure{ERROR_PATH_NOT_FOUND};
    std::filesystem::directory_iterator it(source, ec);
    while (!ec && it != std::filesystem::directory_iterator()) {
        const auto &file = it->path();
        const auto regular = it->is_regular_file(ec);
        const auto target = destination / file.filename();
        if (!ec && regular && !_wcsicmp(file.extension().c_str(), L".dll") && !SameFile(file, target)) {
            // Copy beside it, then rename over it: never write into a DLL another game in this
            // prefix has loaded, and a launch beside it never meets a half-copied file.
            const auto staged = destination / (file.filename().native() + L"." +
                std::to_wstring(GetCurrentProcessId()) + L".new");
            if (!CopyFileW(file.c_str(), staged.c_str(), FALSE) ||
                !MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                const auto error = GetLastError();
                DeleteFileW(staged.c_str());
                throw Failure{error};
            }
        }
        it.increment(ec);
    }
    if (ec) throw Failure{ERROR_PATH_NOT_FOUND};
}
#else
void InstallNetworkingFiles() {}
#endif
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
// The input reader owns stdin after the request; teardown cancels and joins it.
struct RegistrationInput {
    std::mutex mutex;
    Secrets request, code;
};
RegistrationInput registration;
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
    while (!(control.load() & 32) &&
        ReadFile(GetStdHandle(STD_INPUT_HANDLE), &command, 1, &count, nullptr) && count) {
        if (command == 2) break;
        if (command == 5) {
            control.fetch_or(8);
            KillGame();
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
        if (command == 3 && operation != 4) { // Manual sign-in never receives credentials.
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
struct ControlReader {
    Handle thread;
    void start(DWORD operation = 0) {
        thread.value = CreateThread(nullptr, 0, ReadControl,
            reinterpret_cast<void *>(static_cast<ULONG_PTR>(operation)), 0, nullptr);
        if (!thread.value) throw Failure{GetLastError()};
    }
    ~ControlReader() {
        if (!thread.value) return;
        control.fetch_or(32);
        // Cancellation can race the next ReadFile. Repeat until the reader has
        // actually exited, then wipe pending secrets before waiting for GW2.
        while (WaitForSingleObject(thread.value, 0) == WAIT_TIMEOUT) {
            CancelSynchronousIo(thread.value);
            WaitForSingleObject(thread.value, 50);
        }
        std::lock_guard lock(registration.mutex);
        registration.request = {};
        registration.code = {};
    }
};
bool Control(Client &client) {
    const auto state = control.fetch_and(~18u);
    if (state & 4) throw Failure{1000};
    if (!(state & 1)) return false;
    if (state & 2) {
        client.hide = false;
        Reveal(client, true);
    }
    if (state & 16) Report(6, 1008);
    return true;
}
// GW2's answer to a native request, or nothing when it has not answered yet. GW2 answers on
// its window's thread, which loading keeps busy for seconds at a time, before and after its
// first answer (a China client on 2026-10-25: 19 s to its first file write, over two minutes to
// its login page; a second client starting beside running ones). The transport reports that
// as ERROR_TIMEOUT after withdrawing the request, so it never ran: not a failure. The caller
// asks again on a later tick; the phase deadline still ends a game that never answers.
// Failing on it left the game at its login, unsigned and holding the instance lock. The same
// holds for a window GW2 closed since Observe(), or during the call, as its login window does
// at Play (shown as "error 87" while a DLL loaded): the transport reports
// ERROR_INVALID_WINDOW_HANDLE after withdrawing the request, and the next tick finds its
// replacement. `flags` keeps its value until GW2 answers.
std::optional<DWORD> Native(Client &client, DWORD operation, DWORD &flags, const wchar_t *email = nullptr,
    const wchar_t *password = nullptr) {
    DWORD result{}, error{}, answered{};
    // DLLs load on the game window's thread once it exists: an attached WeGame game may
    // keep no live login window.
    const auto window = operation == 8 && client.game ? client.game : client.login ? client.login : client.game;
    const auto transport = GW2_NATIVE_EXECUTE(
        window, client.pid, operation, email, password, &result, &answered, &error, &client.layout);
    if (transport == ERROR_TIMEOUT || transport == ERROR_INVALID_WINDOW_HANDLE) return std::nullopt;
    if (transport) throw Failure{transport};
    flags = answered;
    if (result == 6) throw Failure{error};
    if (result == 9) throw Failure{20000 + error};
    if (result == 3 || result == 5 || result == 0) throw Failure{1100 + result};
    return result;
}
// Whether the login window takes requests yet. GW2 creates it before it has finished loading.
bool LoginReady(Client &client, DWORD &flags) {
    return Native(client, 0, flags) == 1u;
}
// GW2's instance lock refuses every other client while one holds it. A launch the helper
// gives up on leaves its game at the login for the player: release the lock there too, so
// the player's other accounts still launch beside it (failure 1002 otherwise). Best effort:
// a game that does not answer keeps it.
void ReleaseInstanceLock(Client &client) {
    try {
        Observe(client);
        DWORD flags{};
        if (client.login || client.game) (void)Native(client, 3, flags);
    } catch (...) {
    }
}
void ReportFailure(Client &client, HANDLE process, DWORD code) {
    client.hide = false;
    Reveal(client);
    // Before the report: it ends the app's queue hold, and a launch started then must not
    // still meet this lock.
    if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) ReleaseInstanceLock(client);
    try {
        Report(6, code);
        // Failure is not exit: keep the reader/handle alive for Show and Kill,
        // including unexpected exceptions during startup or DLL copying.
        while (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            if (!Control(client)) return;
            Sleep(100);
        }
        Report(5);
    } catch (...) {
        Reveal(client);
    }
}

// The flags in the player's GW2 Settings.json that stop GW2 from patching (patch_flags.hpp),
// 0 for none or no file. The folder is found the way the game finds it, so under Wine it is
// the one in the game's prefix.
DWORD SettingsPatchFlags() {
    wchar_t folder[MAX_PATH]{};
    if (!SHGetSpecialFolderPathW(nullptr, folder, CSIDL_APPDATA, FALSE)) return 0;
    Handle file{CreateFileW((std::wstring(folder) + L"\\Guild Wars 2\\Settings.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
        nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return 0;
    // GW2 keeps 1,024 characters of command line: a file past this size is not a list of flags.
    std::string text(1 << 20, '\0');
    DWORD size{};
    for (DWORD read{}; size < text.size(); size += read)
        if (!ReadFile(file.value, text.data() + size, static_cast<DWORD>(text.size()) - size, &read, nullptr) || !read)
            break;
    text.resize(size);
    return gw2::patch_flags::inSettings(text);
}

// `older`: the launcher found the executable behind the newest build (else it only resumes an
// earlier download).
void Update(const std::wstring &executable, const std::wstring &folder, Client &client, bool older) {
    // Wine starts its persistent desktop lazily. Start it outside the updater
    // job so shell services cannot keep that job alive after patching finishes.
    if (!GetDesktopWindow()) throw Failure{1006};
    // An older executable never gets past these flags: GW2 opens its error window instead of
    // patching, on every launch. Name the flag instead of starting it (1041 -shareArchive,
    // 1042 -noPatch, 1043 both). A current executable only stops when its archive still needs
    // files, which this helper cannot know: that run is judged by how it ends, below.
    const auto patchFlags = SettingsPatchFlags();
    if (older && patchFlags) throw Failure{1040 + patchFlags};
    // The official patcher owns archive writes. Refuse if any client has the
    // archive open; a job tracks self-updater children without killing on close.
    // A folder archive (the China client's) is held through its Lock.dat, which a
    // client that never ran has not created yet.
    {
        const auto path = folder + L"\\Gw2.dat";
        const auto attributes = GetFileAttributesW(path.c_str());
        const bool chunks = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
        Handle archive{CreateFileW(chunks ? (path + L"\\Lock.dat").c_str() : path.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (archive.value == INVALID_HANDLE_VALUE && !(chunks && GetLastError() == ERROR_FILE_NOT_FOUND))
            throw Failure{1005};
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
    if (!GetExitCodeProcess(process.value, &exitCode)) throw Failure{1006};
    // GW2 ends its updater with 0 on every normal end, also a closed window. Another code is
    // its own fatal error (1, after the player closes its error window) or an end from outside.
    if (exitCode) {
        // With a flag that stops patching in the player's settings, GW2 has just said so itself.
        if (patchFlags) throw Failure{1040 + patchFlags};
        Report(17, exitCode); // Carries the code into the launcher's message.
        throw Failure{1006};
    }
    Report(8);
}
BOOL CALLBACK BridgeWindow(HWND window, LPARAM parameter) {
    auto &search = *reinterpret_cast<std::pair<DWORD, DWORD> *>(parameter);
    DWORD pid{};
    const auto thread = GetWindowThreadProcessId(window, &pid);
    wchar_t name[32]{};
    if (pid != search.first || !GetClassNameW(window, name, 32) || wcscmp(name, L"WG2InputWin")) return TRUE;
    search.second = thread;
    return FALSE;
}
// WeGame's bridge looks its game up by file name only: while any other Gw2-64.exe runs,
// such as a global client, it outlives its game, and once WeGame goes offline or exits it
// terminates the first Gw2-64.exe it lists, usually the oldest (GW2WGLauncher.exe,
// 2026-10-24). After its game exits, quit the bridge as it quits itself when it finds no
// game: WM_QUIT to the thread of its WG2InputWin window, which runs its message loop.
struct WeGameBridge {
    const Handle &game;
    Handle process;
    ~WeGameBridge() {
        if (!process.value || !game.value || WaitForSingleObject(game.value, 0) != WAIT_OBJECT_0 ||
            WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
            return;
        std::pair<DWORD, DWORD> search{GetProcessId(process.value), 0};
        EnumWindows(BridgeWindow, reinterpret_cast<LPARAM>(&search));
        if (search.second) PostThreadMessageW(search.second, WM_QUIT, 0, 0);
    }
};
// The running Gw2-64.exe at this path whose parent is WeGame's bridge, or null.
HANDLE FindWeGameClient(const std::wstring &executable, Client &client, WeGameBridge &bridge) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) throw Failure{GetLastError()};
    std::vector<PROCESSENTRY32W> entries;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (auto more = Process32FirstW(snapshot.value, &entry); more; more = Process32NextW(snapshot.value, &entry))
        entries.push_back(entry);
    for (const auto &game : entries) {
        if (_wcsicmp(game.szExeFile, L"Gw2-64.exe")) continue;
        const auto parent = std::find_if(entries.begin(), entries.end(),
            [&](const PROCESSENTRY32W &other) { return other.th32ProcessID == game.th32ParentProcessID; });
        if (parent == entries.end() || _wcsicmp(parent->szExeFile, L"GW2WGLauncher.exe")) continue;
        const auto process = OpenProcess(
            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, game.th32ProcessID);
        if (!process) {
            const auto error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) continue; // Exited since the snapshot.
            // WeGame started it elevated: this helper can neither control nor hook it.
            throw Failure{error == ERROR_ACCESS_DENIED ? 1031u : error};
        }
        std::wstring image(32768, L'\0');
        auto size = static_cast<DWORD>(image.size());
        std::error_code ec;
        if (QueryFullProcessImageNameW(process, 0, image.data(), &size) &&
            std::filesystem::equivalent(image.substr(0, size), executable, ec)) {
            client.pid = game.th32ProcessID;
            bridge.process.value =
                OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent->th32ProcessID);
            return process;
        }
        CloseHandle(process);
    }
    return nullptr;
}
// The WeGame edition of the China client: only WeGame's bridge, GW2WGLauncher.exe beside
// the game, can sign a WeGame account in. It initializes WeGame's Rail SDK, takes the
// account's login from WeGame and starts Gw2-64.exe with it (-email, -password, -nopatchui).
// Started by anyone but WeGame, Rail has WeGame start the bridge again, so the game's parent
// is a bridge either way. Start it and wait for that game; null when cancelled first.
HANDLE AwaitWeGame(
    const std::wstring &executable, const std::wstring &folder, Client &client, WeGameBridge &found) {
    const auto bridge = folder + L"\\GW2WGLauncher.exe";
    auto command = Quote(bridge);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(bridge.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(),
            &startup, &info)) {
        const auto error = GetLastError();
        throw Failure{error == ERROR_ELEVATION_REQUIRED ? 1031u
                : error == ERROR_FILE_NOT_FOUND         ? 1030u
                                                        : error};
    }
    // WeGame owns the bridge: it keeps running beside the game, or hands over to WeGame.
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    Report(15);
    // The player may still have to start or sign in to WeGame: no deadline, Close stops it.
    for (;;) {
        if (!Control(client)) return nullptr;
        if (control.load() & 8) {
            Report(5);
            return nullptr;
        }
        if (const auto game = FindWeGameClient(executable, client, found)) return game;
        Sleep(50);
    }
}
// Wine starts a remote thread without delivering its argument, so a direct
// LoadLibrary call loads nothing. This stub passes the path and stores the
// module address; the thread exit code is not a success signal.
void LoadModule(HANDLE process, const std::wstring &file) {
    const auto load = reinterpret_cast<std::uintptr_t>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!load) throw Failure{ERROR_PROC_NOT_FOUND};
    const auto pathBytes = (file.size() + 1) * sizeof(wchar_t);
    const auto stubAt = (pathBytes + 15) & ~std::size_t{15};
    constexpr std::size_t stubBytes = 64;
    const auto slotAt = (stubAt + stubBytes + 7) & ~std::size_t{7};
    const auto total = slotAt + sizeof(std::uint64_t);
    void *remote = VirtualAllocEx(process, nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) throw Failure{GetLastError()};
    struct Region {
        HANDLE process;
        void *address;
        bool release{};
        ~Region() {
            if (release) VirtualFreeEx(process, address, 0, MEM_RELEASE);
        }
    } region{process, remote};
    std::vector<unsigned char> image(total);
    std::memcpy(image.data(), file.c_str(), pathBytes);
    auto *stub = image.data() + stubAt;
    std::size_t n = 0;
    const auto imm = [&](unsigned char op, std::uintptr_t value) {
        stub[n++] = 0x48;
        stub[n++] = op;
        std::memcpy(stub + n, &value, sizeof(value));
        n += sizeof(value);
    };
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xEC;
    stub[n++] = 0x28;
    imm(0xB9, reinterpret_cast<std::uintptr_t>(remote));
    imm(0xB8, load);
    stub[n++] = 0xFF;
    stub[n++] = 0xD0;
    imm(0xB9, reinterpret_cast<std::uintptr_t>(remote) + slotAt);
    stub[n++] = 0x48;
    stub[n++] = 0x89;
    stub[n++] = 0x01;
    stub[n++] = 0x48;
    stub[n++] = 0x83;
    stub[n++] = 0xC4;
    stub[n++] = 0x28;
    stub[n++] = 0xC3;
    if (n > stubBytes) throw Failure{ERROR_INVALID_PARAMETER};
    SIZE_T transferred{};
    if (!WriteProcessMemory(process, remote, image.data(), total, &transferred) || transferred != total)
        throw Failure{GetLastError()};
    Handle thread{CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<unsigned char *>(remote) + stubAt), nullptr, 0,
        nullptr)};
    if (!thread.value) throw Failure{GetLastError()};
    // Wait for the load itself, however long a scan of the fresh copy or a busy game makes it:
    // it goes on either way, and failing first left the game unsigned. Kill ends the game.
    const HANDLE waits[]{thread.value, process};
    const auto waited = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (waited == WAIT_OBJECT_0 + 1) throw Failure{ERROR_PROCESS_ABORTED};
    if (waited != WAIT_OBJECT_0) throw Failure{GetLastError()};
    region.release = true;
    std::uint64_t module{};
    if (!ReadProcessMemory(process, static_cast<unsigned char *>(remote) + slotAt, &module, sizeof(module),
            &transferred) ||
        transferred != sizeof(module) || module < 0x10000)
        throw Failure{ERROR_MOD_NOT_FOUND};
}
struct Launch {
    // manual: operation 4 leaves sign-in, agreements, and Play to the player, without a deadline.
    enum Phase { starting = 1, signingIn, launching, running, registration = 9, agreement,
        verification, checkingCode, manual };
    Client &client;
    HANDLE process;
    DWORD operation;
    Secrets secret;
    std::vector<std::wstring> dlls;
    DllCopies &copies;
    Phase phase{starting};
    DWORD flags{};
    ULONGLONG deadline{GetTickCount64() + 90000};
    // A game WeGame's bridge started and signs in (AwaitWeGame): with -nopatchui it logs
    // in without GW2's login window, so its instance lock is released and its DLLs load
    // at the game window.
    bool attached{}, unlocked{};
    // When the game window first appeared, for an attached game or a manual launch played
    // before it finished at the login: its lock release and DLL loads end by a deadline from
    // there, as the starting phase does.
    ULONGLONG windowSince{};
    // Each DLL's staged copy, by index, and how many have loaded. A load GW2 did not answer is
    // asked again for the same file.
    std::vector<std::wstring> staged;
    std::size_t loaded{};
    // Kept until GW2 answers.
    Secrets code, retry;
    // The verification GW2 asked for (Report 12/13 detail).
    DWORD challenge{};
    // An add-on in this game asks for a restart to install its update by signalling
    // Local\<channel>.restart.<game pid>: state 16 has the app close the game and launch the
    // account again, which prepares the new add-on first. Open from Running while this helper
    // reports to its app, and closed with this Launch, which ends when the app does: a closed
    // launcher cannot launch again, and the add-on finds no event to signal.
    Handle restart;
    bool restartOpened{};
    enum class Step { done, waiting, closed };
    // A signed-in launch submits its sign-in as soon as GW2's form is ready and loads its DLLs at
    // the game window, as the attached WeGame game does: GW2 signs in and loads character
    // select while they load. A large DLL can take about 10 s to load, which used to come
    // before the sign-in. The manual launch keeps loading at the login form, while the player
    // types; Wine loads at start.
    bool deferred() const { return operation != 4 && !WineHost(); }
    // Releases the instance lock so other clients can launch beside this game; false while GW2
    // has not answered.
    bool unlock() {
        if (unlocked) return true;
        const auto result = Native(client, 3, flags);
        if (!result) return false;
        if (*result != 1) throw Failure{1004};
        return unlocked = true;
    }
    // Submits a sign-in; false while GW2 has not answered.
    bool submit(DWORD request, const Secrets &login) {
        const auto result = Native(client, request, flags, login.name(), login.password());
        if (!result) return false;
        if (*result != 1) throw Failure{1004};
        return true;
    }
    void signIn() {
        if (!unlock() || !submit(operation == 3 ? 7u : operation == 2 ? 5u : 1u, secret)) return;
        SecureZeroMemory(secret.bytes.data(), secret.bytes.size());
        Report(2);
        phase = signingIn;
        deadline = GetTickCount64() + 120000;
    }
    void setup() {
        if (retry.bytes.empty()) retry = TakeCredentials();
        if (retry.bytes.empty()) return;
        if (operation < 2 || !retry.name()[0]) throw Failure{1004};
        if (!submit(operation == 3 ? 7u : 5u, retry)) return;
        retry = {};
        Report(2);
        phase = signingIn;
        deadline = GetTickCount64() + 120000;
    }
    void signedIn() {
        if (client.game) {
            if (!dlls.empty()) {
                // Deferred DLLs load at this window first; observe() reports Running after them.
                Report(3);
                phase = launching;
                deadline = GetTickCount64() + 90000;
                return;
            }
            Reveal(client);
            Report(4);
            phase = running;
            return;
        }
        if (!client.login) throw Failure{1004};
        const auto result = Native(client, 4, flags);
        if (!result) return;
        if (*result == 12) {
            challenge = (flags >> 24) & 7;
            if (phase != verification) Report(12, challenge);
            phase = verification;
        } else if (*result == 8 && operation >= 2) {
            Report(9);
            phase = registration;
        } else if (*result == 10) {
            client.hide = false;
            Reveal(client, true);
            Report(10);
            phase = agreement;
        } else if (*result == 7) {
            Report(3);
            phase = launching;
            deadline = GetTickCount64() + 90000;
        } else if (*result != 1 || !(flags & (2 | 4)))
            throw Failure{1004};
    }
    void verify() {
        if (code.bytes.empty()) code = TakeCode();
        if (code.bytes.empty()) return;
        const auto result = Native(client, 9, flags, nullptr,
            reinterpret_cast<const wchar_t *>(code.bytes.data()));
        if (!result) return;
        code = {};
        if (*result != 1 && *result != 4) throw Failure{1004};
        Report(13, challenge);
        phase = checkingCode;
        deadline = GetTickCount64() + 120000;
    }
    // Loads the DLLs in order, each once.
    Step loadDlls() {
        for (; loaded < dlls.size(); ++loaded) {
            if (!Control(client)) {
                Reveal(client);
                return Step::closed;
            }
            const auto index = static_cast<DWORD>(loaded + 1);
            if (staged.size() == loaded) Report(11, index);
            try {
                if (staged.size() == loaded) {
                    staged.push_back(copies.copy(dlls[loaded]));
                    if (!Control(client)) {
                        Reveal(client);
                        return Step::closed;
                    }
                }
                if (WineHost())
                    LoadModule(process, staged[loaded]);
                else {
                    const auto directory = std::filesystem::path(dlls[loaded]).parent_path().native();
                    const auto result = Native(client, 8, flags, directory.c_str(), staged[loaded].c_str());
                    if (!result) return Step::waiting;
                    if (*result != 1) throw Failure{ERROR_DLL_INIT_FAILED};
                }
            } catch (const Failure &failure) {
                throw Failure{0x40000000u | (index << 16) | (failure.code > 0xFFFF ? 0xFFFF : failure.code)};
            }
        }
        if (!dlls.empty()) deadline = GetTickCount64() + 90000;
        dlls.clear();
        staged.clear();
        loaded = 0;
        return Step::done;
    }
    void observe() {
        // If native acceptance needs attention, reveal its prompt and observe
        // continuation without repeatedly accepting or invoking Play.
        if (phase == agreement && !client.game && (!client.login || Native(client, 6, flags) == 7u)) {
            Report(3);
            phase = launching;
            deadline = GetTickCount64() + 90000;
        }
        // Running releases the next queued launch: a game first loads its DLLs, and an attached
        // one releases its instance lock.
        if ((phase == launching || phase == agreement || phase == manual) && client.game && dlls.empty() &&
            (!attached || unlocked)) {
            Reveal(client);
            Report(4);
            phase = running;
        }
        if ((phase < running || phase == checkingCode) && GetTickCount64() > deadline)
            throw Failure{WAIT_TIMEOUT};
    }
    void pollRestart() {
        if (!std::exchange(restartOpened, true)) {
            wchar_t name[64]{};
            swprintf_s(name, L"Local\\" GW2_NATIVE_CHANNEL_W L".restart.%lu", client.pid);
            restart.value = CreateEventW(nullptr, FALSE, FALSE, name);
        }
        if (restart.value && WaitForSingleObject(restart.value, 0) == WAIT_OBJECT_0) Report(16);
    }
    void run() {
        while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            if (!Control(client)) {
                Reveal(client);
                return;
            }
            Observe(client);
            // Wine reports the login window disabled before a hook can run, and that
            // used to return before any addon was loaded.
            const auto wine = WineHost();
            // A manual launch's player may sign in and press Play while its DLLs load at the
            // login (a large one takes about 10 s), and GW2 then closes the login window: the
            // launch finishes at the game window, as an attached game does, with its own
            // deadline from that window. Dialogs there are the player's, as in `manual`.
            const bool played = phase == starting && operation == 4 && !client.login && client.game;
            if (played && !windowSince) {
                windowSince = GetTickCount64();
                deadline = windowSince + 90000;
            }
            if (phase == starting && wine && !dlls.empty() && (client.login || client.game) &&
                loadDlls() == Step::closed)
                return;
            // Release the instance lock so other clients can launch beside this game. Before
            // Running: the queue holds the next launch until then, so a window that never
            // answers still ends.
            if (attached && client.game) {
                if (!windowSince) windowSince = GetTickCount64();
                if ((!unlocked || !dlls.empty()) && GetTickCount64() - windowSince > 90000)
                    throw Failure{WAIT_TIMEOUT};
                if (unlock() && !dlls.empty() && loadDlls() == Step::closed) return;
            }
            // A login window closed since Observe() reads as disabled: only one that still
            // exists needs attention.
            if (phase != running && phase != manual && !played &&
                (client.dialog ||
                    (!wine && client.login && !IsWindowEnabled(client.login) && IsWindow(client.login))))
                throw Failure{1003};
            if (client.hide && (phase < running || phase == registration || phase == verification ||
                    phase == checkingCode) && client.login &&
                IsWindowVisible(client.login))
                ShowWindowAsync(client.login, SW_HIDE);
            // Loading GW2 and fetching a platform token overlap. Only validated
            // credentials from the pipe reader may cross into native sign-in.
            if (phase == starting && secret.bytes.empty()) secret = TakeCredentials();
            if (phase == starting && (played || (client.login && LoginReady(client, flags)))) {
                const auto step = wine || deferred() ? Step::done : loadDlls();
                if (step == Step::closed) return;
                if (step == Step::done) {
                    if (!Control(client)) {
                        Reveal(client);
                        return;
                    }
                    if (!secret.bytes.empty()) signIn();
                    // Release the instance lock so accounts can launch beside this game.
                    if (operation == 4 && unlock()) {
                        // With the game window open, observe() reports Running at once.
                        if (!client.game) Report(2);
                        phase = manual;
                    }
                }
            }
            if (phase == registration) setup();
            if (phase == verification) verify();
            if (phase == signingIn || phase == verification || phase == checkingCode) signedIn();
            if ((phase == launching || phase == agreement) && client.game && !dlls.empty() && deferred() &&
                loadDlls() == Step::closed)
                return;
            observe();
            if (phase == running) pollRestart();
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

extern "C" int main(int argc, char **argv) {
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "--input"))) return 2;
    Client client;
    Handle process, input;
    // A launcher closed before its game: wait for the game, then record when it closed. Destroyed
    // after the proxy session, bridge and copies, which keep their own order.
    struct PlayedOnExit {
        const Handle &process;
        ~PlayedOnExit() {
            if (process.value && reachedGame && !playedWritten &&
                WaitForSingleObject(process.value, INFINITE) == WAIT_OBJECT_0)
                RecordPlayed();
        }
    } playedOnExit{process};
    gw2_proxy::Session proxySession;
    // Destroyed after the copies, whose cleanup waits for the game to exit.
    WeGameBridge bridge{process};
    DllCopies copies{process};
    ControlReader reader;
    try {
        if (argc == 3) OpenInput(input, argv[2]);
        if (Number() != Magic) throw Failure{1000};
        auto operation = Number();
        const bool proxied = operation == 16 || operation == 18 || operation == 19;
        if (proxied) operation -= 16;
        if (operation > 5) throw Failure{1000};
        // 5: the normal launch of a WeGame install, started by WeGame's bridge (AwaitWeGame).
        const bool wegame = operation == 5;
        if (wegame) operation = 4;
        if (proxied && WineHost()) throw Failure{1023};
        const auto executable = DosPath(String());
        const auto folder = executable.substr(0, executable.find_last_of(L"\\/"));
        if (operation == 1) {
            const bool older = Number() != 0; // Read before the reader takes over the input.
            reader.start();
            Update(executable, folder, client, older);
            return 0;
        }
        try {
            client.layout =
                gw2::Image(std::filesystem::path(executable)).resolve(operation == 2 || operation == 3 ? operation - 1 : 0);
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
        // Up to 16 user DLLs plus one launcher-managed client.
        if (dllCount > 17) throw Failure{1000};
        std::vector<std::wstring> dlls;
        for (DWORD i = 0; i < dllCount; ++i) {
            auto dll = DosPath(String());
            if (dll.empty() || dll.size() >= 4096 || !std::filesystem::path(dll).is_absolute() ||
                _wcsicmp(std::filesystem::path(dll).extension().c_str(), L".dll"))
                throw Failure{1000};
            dlls.push_back(std::move(dll));
        }
        // Empty for the normal launch. The launcher made its folder; the file may not exist yet.
        if (const auto played = String(); !played.empty()) {
            const auto slash = played.find_last_of(L"\\/");
            if (slash == played.npos || !played.ends_with(L".txt")) throw Failure{1000};
            playedFile = DosPath(played.substr(0, slash)) + L"\\" + played.substr(slash + 1);
            if (!std::filesystem::path(playedFile).is_absolute()) throw Failure{1000};
        }
        std::wstring mitmdump, proxyScript;
        Secrets proxyConfig;
        if (proxied) {
            mitmdump = String();
            proxyScript = String();
            if (!std::filesystem::path(mitmdump).is_absolute() ||
                !std::filesystem::path(proxyScript).is_absolute()) throw Failure{1020};
            const auto size = Number();
            if (!size || size > 8192) throw Failure{1000};
            proxyConfig.bytes.resize(size);
            Read(proxyConfig.bytes.data(), size);
        }
        Handle existing{OpenMutexW(SYNCHRONIZE, FALSE, L"AN-Mutex-Window-Guild Wars 2")};
        if (existing.value) throw Failure{1002};
        if (GetLastError() != ERROR_FILE_NOT_FOUND) throw Failure{GetLastError()};
        reader.start(operation);
        if (!Control(client)) return 0;
        if (control.load() & 8) {
            Report(5);
            return 0;
        }
        InstallNetworkingFiles();
        if (wegame) {
            process.value = AwaitWeGame(executable, folder, client, bridge);
            if (!process.value) return 0;
            gameProcess.store(process.value);
            KillGame();
            Report(1, client.pid);
            // WeGame signs it in: no credentials and no login window.
            Report(2);
            Launch launch{client, process.value, operation, {}, std::move(dlls), copies};
            launch.phase = Launch::manual;
            launch.attached = true;
            launch.run();
            return 0;
        }
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = client.hide ? SW_HIDE : SW_SHOWNORMAL;
        PROCESS_INFORMATION info{};
        // No pipe handles inherited by GW2: only this helper can read account secrets.
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                proxied ? CREATE_SUSPENDED : 0, nullptr,
                folder.c_str(), &startup, &info))
            throw Failure{GetLastError()};
        process.value = info.hProcess;
        gameProcess.store(process.value);
        KillGame();
        Handle primaryThread{info.hThread};
        client.pid = info.dwProcessId;
        Report(1, client.pid); // Starting: detail identifies the game, not this helper.
        if (proxied) {
            proxySession.start(process.value, client.pid, mitmdump, proxyScript, proxyConfig.bytes,
                [&] { return !(control.load() & 1) || (control.load() & (8 | 32)) ||
                    WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT; },
                [](DWORD code) { Report(6, code); });
            proxyConfig = {};
            if (ResumeThread(primaryThread.value) == static_cast<DWORD>(-1)) {
                TerminateProcess(process.value, ERROR_PROCESS_ABORTED);
                throw Failure{1020};
            }
        }
        Launch{client, process.value, operation, {}, std::move(dlls), copies}.run();
    } catch (const gw2_proxy::Failure &failure) {
        ReportFailure(client, process.value, failure.code);
        return 1;
    } catch (const Failure &failure) {
        ReportFailure(client, process.value, failure.code);
        return 1;
    } catch (...) {
        ReportFailure(client, process.value, 1000);
        return 1;
    }
    return 0;
}

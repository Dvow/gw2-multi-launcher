module;
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

export module uninstall_stub;

namespace {
constexpr char kMagic[] = {'K', 'X', 'U', 'N', 'I', 'N', 'S', 'T'};

struct Footer {
    char magic[8]{};
    std::uint64_t exeBytes{};
    std::uint64_t datBytes{};
};
static_assert(sizeof(Footer) == 24);

const wchar_t *skipFirst(const wchar_t *command) {
    if (!command || !*command) return L"";
    if (*command == L'"') {
        ++command;
        while (*command && *command != L'"') ++command;
        if (*command == L'"') ++command;
    } else {
        while (*command && *command != L' ' && *command != L'\t') ++command;
    }
    while (*command == L' ' || *command == L'\t') ++command;
    return command;
}

bool readAt(HANDLE file, std::uint64_t offset, void *data, std::uint64_t size) {
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) return false;
    auto *out = static_cast<std::uint8_t *>(data);
    std::uint64_t done = 0;
    while (done < size) {
        DWORD chunk = 0;
        const auto ask = static_cast<DWORD>(size - done > 0x100000 ? 0x100000 : size - done);
        if (!ReadFile(file, out + done, ask, &chunk, nullptr) || chunk == 0) return false;
        done += chunk;
    }
    return true;
}

bool writeAll(const std::wstring &path, const std::vector<std::uint8_t> &bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::uint64_t done = 0;
    bool ok = true;
    while (ok && done < bytes.size()) {
        DWORD chunk = 0;
        const auto ask = static_cast<DWORD>(bytes.size() - done > 0x100000 ? 0x100000 : bytes.size() - done);
        ok = WriteFile(file, bytes.data() + done, ask, &chunk, nullptr) && chunk != 0;
        done += chunk;
    }
    CloseHandle(file);
    return ok && done == bytes.size();
}

std::wstring quote(const std::wstring &value) { return L"\"" + value + L"\""; }

void removeInstall() {
    wchar_t path[32768];
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (length == 0 || length >= 32768) return;
    const std::wstring file(path, length);
    const auto slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const auto folder = file.substr(0, slash);
    std::wstring command = L"cmd.exe /d /c \"for /L %I in (1,1,30) do @(del /f /q ";
    command += quote(file);
    command += L" & if not exist ";
    command += quote(file);
    command += L" (rmdir /s /q ";
    command += quote(folder);
    command += L" & exit /b 0) & ping -n 2 127.0.0.1 >nul)\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &startup, &process)) {
        process = {};
        CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
            nullptr, &startup, &process);
    }
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (!process.hProcess) MoveFileExW(file.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

int fail(const std::wstring &dir, const std::wstring &exe, const std::wstring &dat) {
    if (!exe.empty()) DeleteFileW(exe.c_str());
    if (!dat.empty()) DeleteFileW(dat.c_str());
    if (!dir.empty()) RemoveDirectoryW(dir.c_str());
    MessageBoxA(nullptr, "Could not start uninstall.", GW2_PRODUCT_NAME, MB_ICONERROR | MB_OK);
    return 1;
}
} // namespace

extern "C" int main() {
    wchar_t self[32768];
    const auto length = GetModuleFileNameW(nullptr, self, 32768);
    if (length == 0 || length >= 32768) return fail({}, {}, {});
    HANDLE file = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail({}, {}, {});
    LARGE_INTEGER bytes{};
    Footer footer;
    const bool header = GetFileSizeEx(file, &bytes) && bytes.QuadPart >= static_cast<LONGLONG>(sizeof(footer)) &&
        readAt(file, static_cast<std::uint64_t>(bytes.QuadPart) - sizeof(footer), &footer, sizeof(footer));
    if (!header || std::char_traits<char>::compare(footer.magic, kMagic, 8) != 0 || footer.exeBytes == 0 ||
        footer.datBytes == 0 ||
        footer.exeBytes + footer.datBytes + sizeof(footer) > static_cast<std::uint64_t>(bytes.QuadPart)) {
        CloseHandle(file);
        return fail({}, {}, {});
    }
    const auto exeAt = static_cast<std::uint64_t>(bytes.QuadPart) - sizeof(footer) - footer.datBytes - footer.exeBytes;
    const auto datAt = exeAt + footer.exeBytes;
    std::vector<std::uint8_t> exeBytes(static_cast<std::size_t>(footer.exeBytes));
    std::vector<std::uint8_t> datBytes(static_cast<std::size_t>(footer.datBytes));
    const bool loaded = readAt(file, exeAt, exeBytes.data(), footer.exeBytes) &&
        readAt(file, datAt, datBytes.data(), footer.datBytes);
    CloseHandle(file);
    if (!loaded) return fail({}, {}, {});
    wchar_t temp[MAX_PATH];
    const auto tempLength = GetTempPathW(MAX_PATH, temp);
    if (tempLength == 0 || tempLength >= MAX_PATH) return fail({}, {}, {});
    const auto dir = std::wstring(temp) + L"gw2-uninst-" + std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(dir.c_str(), nullptr)) return fail({}, {}, {});
    const auto exe = dir + L"\\unins000.exe";
    const auto dat = dir + L"\\unins000.dat";
    if (!writeAll(exe, exeBytes) || !writeAll(dat, datBytes)) return fail(dir, exe, dat);
    const auto *extra = skipFirst(GetCommandLineW());
    std::wstring command = L"\"" + exe + L"\"";
    if (extra && *extra) {
        command.push_back(L' ');
        command += extra;
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup,
            &process))
        return fail(dir, exe, dat);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    DeleteFileW(exe.c_str());
    DeleteFileW(dat.c_str());
    RemoveDirectoryW(dir.c_str());
    removeInstall();
    return static_cast<int>(code);
}

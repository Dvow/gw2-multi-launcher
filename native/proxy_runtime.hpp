#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sddl.h>
#include "process_exit.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace gw2_proxy {
struct Failure { DWORD code; };
struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
inline std::wstring quote(std::wstring_view text) {
    std::wstring result = L"\"";
    std::size_t slashes{};
    for (auto c : text) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
inline std::wstring serviceName() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) throw Failure{1020};
    DWORD size{};
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> data(size);
    if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size)) throw Failure{1020};
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid)) throw Failure{1020};
    std::wstring result = L"KXProxy-1-" + std::wstring(sid);
    LocalFree(sid);
    DWORD session{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) throw Failure{1020};
    return result + L"-" + std::to_wstring(session);
}

// Shared service, separate private control connection and job for each game.
// The UI may close: this object keeps its host alive until GW2 exits.
class Session {
    Handle pipe_, job_;
    HANDLE game_{}; // borrowed, outlives this session
    std::jthread monitor_;
    bool active_{};
    std::string incoming_;
    ULONGLONG lastPulse_{};
    bool ready_{}, probe_{};

    void messages() {
        DWORD available{};
        if (!PeekNamedPipe(pipe_.value, nullptr, 0, nullptr, &available, nullptr)) throw Failure{1022};
        while (available) {
            std::array<char, 256> buffer{};
            DWORD count{};
            if (!ReadFile(pipe_.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || !count)
                throw Failure{1022};
            incoming_.append(buffer.data(), count);
            if (incoming_.size() > 1024) throw Failure{1022};
            for (auto end = incoming_.find('\n'); end != incoming_.npos; end = incoming_.find('\n')) {
                auto message = incoming_.substr(0, end);
                incoming_.erase(0, end + 1);
                if (message == "KXPROXY READY") ready_ = true;
                else if (message == "KXPROXY PROBE") probe_ = true;
                else if (message == "KXPROXY ERROR 1020") throw Failure{1020};
                else if (message == "KXPROXY ERROR 1021") throw Failure{1021};
                else if (message == "KXPROXY ERROR 1022") throw Failure{1022};
                else if (message != "KXPROXY ALIVE") throw Failure{1022};
                lastPulse_ = GetTickCount64();
            }
            if (!PeekNamedPipe(pipe_.value, nullptr, 0, nullptr, &available, nullptr)) throw Failure{1022};
        }
    }

    static void startService(const std::wstring &executable, const std::wstring &script, const std::wstring &pipe) {
        std::wstring command = quote(executable) + L" -q --set server=false --set flow_detail=0 --set termlog_verbosity=error -s " +
            quote(script) + L" --set " + quote(L"kx_proxy_pipe=" + pipe);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION info{};
        // No inherited stdin/stdout, secrets, console, shell command, or system proxy changes.
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &info)) throw Failure{1020};
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess); // Shared service owns its own lifetime.
    }

    void verifyCapture(const std::function<bool()> &cancelled, ULONGLONG deadline) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw Failure{1020};
        struct Network { ~Network() { WSACleanup(); } } network;
        struct Socket {
            SOCKET value{INVALID_SOCKET};
            ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
        } socket;
        socket.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket.value == INVALID_SOCKET) throw Failure{1020};
        u_long nonblocking = 1;
        if (ioctlsocket(socket.value, FIONBIO, &nonblocking)) throw Failure{1020};
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(9);
        InetPtonW(AF_INET, L"198.18.0.254", &destination.sin_addr);
        // A unique connection from this host proves that the PID filter reached
        // the Windows driver. set_intercept() itself only queues that change.
        if (connect(socket.value, reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) &&
            WSAGetLastError() != WSAEWOULDBLOCK) throw Failure{1020};
        std::array<char, 8> reply{};
        std::size_t received{};
        while (received != reply.size() || !ready_) {
            if (cancelled() || GetTickCount64() >= deadline) throw Failure{1020};
            messages();
            if (received < reply.size()) {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(socket.value, &readable);
                timeval timeout{};
                const auto available = select(0, &readable, nullptr, nullptr, &timeout);
                if (available == SOCKET_ERROR) throw Failure{1020};
                if (available) {
                    const auto count = recv(socket.value, reply.data() + received,
                        static_cast<int>(reply.size() - received), 0);
                    if (count > 0) received += static_cast<std::size_t>(count);
                    else if (!count || WSAGetLastError() != WSAEWOULDBLOCK) throw Failure{1020};
                }
            }
            Sleep(20);
        }
        if (std::string_view(reply.data(), reply.size()) != "KXPROXY\n") throw Failure{1020};
    }

  public:
    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    ~Session() {
        if (active_) WaitForSingleObject(game_, INFINITE);
        monitor_.request_stop();
        if (monitor_.joinable()) monitor_.join();
        // Job closes before the control pipe: a crashed host cannot release a
        // running client into a direct route. The service also waits on its PID.
    }
    void start(HANDLE game, DWORD pid, const std::wstring &executable, const std::wstring &script,
        std::span<const BYTE> config, const std::function<bool()> &cancelled,
        std::function<void(DWORD)> report) {
        game_ = game;
        try {
            job_.value = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job_.value || !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation,
                    &limits, sizeof(limits)) || !AssignProcessToJobObject(job_.value, game))
                throw Failure{1020};
            const auto name = serviceName();
            Handle mutex{CreateMutexW(nullptr, FALSE, (L"Local\\" + name).c_str())};
            if (!mutex.value) throw Failure{1020};
            const auto deadline = GetTickCount64() + 60000;
            for (;;) {
                const auto result = WaitForSingleObject(mutex.value, 50);
                if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) break;
                if (result != WAIT_TIMEOUT || cancelled() || GetTickCount64() > deadline) throw Failure{1020};
            }
            struct Unlock { HANDLE value; ~Unlock() { ReleaseMutex(value); } } unlock{mutex.value};
            const auto pipeName = L"\\\\.\\pipe\\" + name;
            bool started{};
            for (;;) {
                pipe_.value = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
                if (pipe_.value != INVALID_HANDLE_VALUE) break;
                const auto error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) throw Failure{1020};
                if (cancelled() || GetTickCount64() > deadline) throw Failure{1020};
                if (!started && error == ERROR_FILE_NOT_FOUND) {
                    startService(executable, script, pipeName);
                    started = true;
                }
                Sleep(50);
            }
            std::string request = "{\"pid\":" + std::to_string(pid) + ",\"owner\":" +
                std::to_string(GetCurrentProcessId()) + ",\"proxy\":";
            struct Wipe { std::string &text; ~Wipe() { SecureZeroMemory(text.data(), text.size()); } } wipe{request};
            request.reserve(request.size() + config.size() + 1);
            request.append(reinterpret_cast<const char *>(config.data()), config.size());
            request += '}';
            DWORD written{};
            if (!WriteFile(pipe_.value, request.data(), static_cast<DWORD>(request.size()), &written, nullptr) ||
                written != request.size()) throw Failure{1020};
            lastPulse_ = GetTickCount64();
            while (!probe_) {
                if (cancelled() || GetTickCount64() > deadline) throw Failure{1020};
                messages();
                Sleep(20);
            }
            verifyCapture(cancelled, deadline);
            monitor_ = std::jthread([this, report = std::move(report)](std::stop_token stop) {
                while (!stop.stop_requested() && WaitForSingleObject(game_, 100) == WAIT_TIMEOUT) {
                    try {
                        messages();
                        if (GetTickCount64() - lastPulse_ > 5000) throw Failure{1022};
                    } catch (const Failure &failure) {
                        if (TerminateJobObject(job_.value, ERROR_CONNECTION_ABORTED))
                            gw2_exit::hasten(GetProcessId(game_));
                        try { report(failure.code); } catch (...) {}
                        return;
                    }
                }
            });
            active_ = true;
        } catch (...) {
            TerminateProcess(game_, ERROR_CONNECTION_ABORTED);
            throw;
        }
    }
};
} // namespace gw2_proxy

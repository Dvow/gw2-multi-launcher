// Explicit Windows driver integration probe, never an automatic CTest.
// Runs suspended synthetic clients; does not launch GW2 or contact a game server.
#include "../native/proxy_runtime.hpp"
#include <cstdio>

int client(const std::string &tag) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 10;
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return 11;
    DWORD timeout = 15000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(6112);
    InetPtonW(AF_INET, L"203.0.113.7", &address.sin_addr);
    if (connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address))) return 12;
    for (const std::string payload : {"first", "second"}) {
        if (send(socket, payload.data(), static_cast<int>(payload.size()), 0) != static_cast<int>(payload.size())) return 13;
        const auto expected = tag + ":" + payload;
        std::string reply(expected.size(), '\0');
        std::size_t offset{};
        while (offset < reply.size()) {
            const auto read = recv(socket, reply.data() + offset, static_cast<int>(reply.size() - offset), 0);
            if (read <= 0) { Sleep(7000); return 14; } // Wait for the host's failure watchdog.
            offset += read;
        }
        if (reply != expected) return 15;
        Sleep(tag == "B" && payload == "first" ? 3500 : 1200);
    }
    closesocket(socket);
    const auto datagram = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (datagram == INVALID_SOCKET) return 16;
    timeout = 1500;
    setsockopt(datagram, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    if (sendto(datagram, "udp", 3, 0, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 3) return 17;
    char response[16]{};
    const auto received = recv(datagram, response, sizeof(response), 0);
    const bool udpOk = tag == "A" ? received == 5 && std::string_view(response, 5) == "A:udp"
                                  : received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT;
    closesocket(datagram);
    WSACleanup();
    return udpOk ? 0 : 18;
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--client")
        return client(argv[2][0] == L'A' ? "A" : "B");
    if (argc != 6) return 2;
    const auto tag = std::wstring(argv[5]);
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    auto command = gw2_proxy::quote(executable) + L" --client " + tag;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &info)) return 3;
    gw2_proxy::Handle game{info.hProcess}, thread{info.hThread};
    gw2_proxy::Session session;
    const auto port = std::to_string(std::stoul(argv[4]));
    const std::string protocol = std::wstring_view(argv[3]) == L"http" ? "http" : "socks5";
    const auto config = "{\"protocol\":\"" + protocol + "\",\"host\":\"127.0.0.1\",\"port\":" + port +
        ",\"username\":\"\",\"password\":\"\"}";
    std::atomic<DWORD> failure{};
    try {
        session.start(game.value, info.dwProcessId, argv[1], argv[2],
            {reinterpret_cast<const BYTE *>(config.data()), config.size()}, [] { return false; },
            [&](DWORD code) { failure.store(code); });
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) throw gw2_proxy::Failure{1020};
        if (WaitForSingleObject(game.value, 30000) != WAIT_OBJECT_0) {
            TerminateProcess(game.value, 20);
            return 20;
        }
        DWORD code{};
        GetExitCodeProcess(game.value, &code);
        if (failure.load()) {
            std::printf("Proxy failure: %lu\n", failure.load());
            return 21;
        }
        std::printf("Client %ls exit: %lu\n", tag.c_str(), code);
        return static_cast<int>(code);
    } catch (const gw2_proxy::Failure &error) {
        TerminateProcess(game.value, error.code);
        std::printf("Proxy startup failed: %lu\n", error.code);
        return 22;
    }
}

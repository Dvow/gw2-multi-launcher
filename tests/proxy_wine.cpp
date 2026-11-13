// Optional real Wine/Proton integration client. No GW2 or Internet server needed.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

bool tcp(const char *address, int family, char tag) {
    const auto s = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD timeout = 10000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    sockaddr_storage storage{};
    int size{};
    if (family == AF_INET) {
        auto &v4 = reinterpret_cast<sockaddr_in &>(storage);
        v4.sin_family = AF_INET; v4.sin_port = htons(6112);
        inet_pton(family, address, &v4.sin_addr); size = sizeof(v4);
    } else {
        auto &v6 = reinterpret_cast<sockaddr_in6 &>(storage);
        v6.sin6_family = AF_INET6; v6.sin6_port = htons(6112);
        inet_pton(family, address, &v6.sin6_addr); size = sizeof(v6);
    }
    bool ok = !connect(s, reinterpret_cast<sockaddr *>(&storage), size) && send(s, "wine", 4, 0) == 4;
    char response[6]{};
    for (int offset = 0; ok && offset < 6;) {
        const auto n = recv(s, response + offset, 6 - offset, 0);
        if (n <= 0) ok = false; else offset += n;
    }
    closesocket(s);
    return ok && response[0] == tag && std::memcmp(response + 1, ":wine", 5) == 0;
}
int main(int argc, char **argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 10;
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0)
        return tcp("203.0.113.7", AF_INET, argv[2][0]) ? 0 : 11;
    if (argc != 5 || std::strcmp(argv[3], "--input")) return 12;
    using Convert = WCHAR *(__cdecl *)(const char *);
    const auto convert = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
    auto name = convert ? convert(argv[4]) : nullptr;
    if (!name) return 13;
    const auto input = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HeapFree(GetProcessHeap(), 0, name);
    char secret[19]{};
    DWORD count{};
    const bool valid = input != INVALID_HANDLE_VALUE && ReadFile(input, secret, sizeof(secret), &count, nullptr) &&
        count == sizeof(secret) && std::memcmp(secret, "secret-through-fifo", sizeof(secret)) == 0;
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!valid) return 14;
    const auto tag = argv[1][0];
    if (!tcp("203.0.113.7", AF_INET, tag) || !tcp("2001:db8::7", AF_INET6, tag)) return 15;
    addrinfo hints{}, *addresses{};
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("test.invalid", "6112", &hints, &addresses)) return 16;
    const auto resolved = reinterpret_cast<sockaddr_in *>(addresses->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(addresses);
    in_addr expected{};
    inet_pton(AF_INET, "203.0.113.7", &expected);
    if (resolved != expected.s_addr) return 17;
    const auto udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD timeout = 2000;
    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    sockaddr_in destination{};
    destination.sin_family = AF_INET; destination.sin_port = htons(6112); destination.sin_addr.s_addr = resolved;
    if (sendto(udp, "wine-udp", 8, 0, reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) != 8) return 18;
    char reply[16]{};
    const auto received = recv(udp, reply, sizeof(reply), 0);
    const bool udpOk = std::strcmp(argv[2], "socks5") == 0
        ? received == 8 && std::memcmp(reply, "wine-udp", 8) == 0
        : received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT;
    closesocket(udp);
    if (!udpOk) return 19;
    char executable[32768]{};
    if (!GetModuleFileNameA(nullptr, executable, sizeof(executable))) return 20;
    auto command = std::string("\"") + executable + "\" --child " + tag;
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessA(executable, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child)) return 21;
    if (WaitForSingleObject(child.hProcess, 20000) != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 22); return 22; }
    DWORD result{};
    GetExitCodeProcess(child.hProcess, &result);
    CloseHandle(child.hProcess); CloseHandle(child.hThread);
    WSACleanup();
    if (result) return 23;
    std::puts("PROBE OK");
    return 0;
}

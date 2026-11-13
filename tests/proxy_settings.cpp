#include "../src/proxy_settings.hpp"
#include <iostream>

int main() {
    unsigned checks{};
    const auto check = [&](gw2_proxy::Settings value, bool valid) {
        bool accepted = true;
        try { gw2_proxy::validate(value); } catch (const std::runtime_error &) { accepted = false; }
        if (accepted != valid) throw std::runtime_error("Proxy setting validation failed");
        ++checks;
    };
    gw2_proxy::Settings proxy;
    check(proxy, true); // Existing accounts are direct, with no required endpoint.
    proxy.enabled = true;
    check(proxy, false);
    proxy.host = "proxy.example.com";
    check(proxy, true);
    proxy.host = "2001:db8::1";
    check(proxy, true);
    for (const auto host : {"https://proxy.example.com", "name@host", "host\r\nInjected", "[::1]", "host/path"}) {
        proxy.host = host;
        check(proxy, false);
    }
    proxy.host = "127.0.0.1";
    for (const auto port : {0u, 65536u, 0xffffffffu}) {
        proxy.port = port;
        check(proxy, false);
    }
    proxy.port = 65535;
    for (const auto protocol : {"socks5", "http", "https"}) {
        proxy.protocol = protocol;
        check(proxy, true);
    }
    proxy.protocol = "socks4";
    check(proxy, false);
    proxy.protocol = "socks5";
    proxy.username = "username:password";
    check(proxy, false);
    proxy.username = "user";
    proxy.protectedPassword.assign(16385, 'a');
    check(proxy, false);
    bool rejected{};
    try { gw2_proxy::validatePassword(std::string(256, 'x')); }
    catch (const std::runtime_error &) { rejected = true; }
    if (!rejected) throw std::runtime_error("Oversized password accepted");
    std::cout << checks + 1 << " proxy settings checks passed\n";
}

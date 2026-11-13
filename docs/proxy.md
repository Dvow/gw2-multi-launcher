# Per-account GW2 proxies

Each account can select a different SOCKS5, HTTP CONNECT, or HTTPS CONNECT proxy. Accounts without the option enabled continue to connect normally. The proxy carries opaque game bytes, without installing certificates or decrypting TLS.

## Windows setup

1. Install [mitmproxy 12.2](https://www.mitmproxy.org/) or a compatible newer release, including `mitmdump.exe` and its Windows local-capture helper. The implementation has been tested against mitmproxy 12.2.0 / mitmproxy-rs 0.12.11.
2. In launcher **Settings**, enter the full path to `mitmdump.exe`. Leave this empty if the executable is beside the launcher or on `PATH`. The two `kx_proxy*.py` files shipped with the launcher must remain beside the launcher executable.
3. Edit an account and enable **Use a proxy for this account**. Choose SOCKS5, HTTP CONNECT, or HTTPS CONNECT; enter the host, port, and optional username/password. Enter IPv6 addresses without brackets. Save the account.
4. Repeat for other accounts with their own endpoints. Launch normally. Windows may ask to allow mitmproxy's elevated capture helper. Canceling or failing setup stops this launch.

An account with the option disabled uses its normal connection. Changing settings affects the next launch. The password field reopens blank with **Password saved** beneath it; leave it blank to retain the saved password. Enter a replacement to change it, or choose **Remove saved proxy password** to delete it. Editing or clearing the username and disabling the proxy keep the saved password. No credentials are sent while the username is empty. Passwords use the launcher's existing OS-backed encryption and are sent to the local capture service over a same-user named pipe. They are never placed in command-line arguments or plaintext configuration files.

## Linux / Wine / Proton setup

1. Install the Linux version of **mitmproxy 12.2 or newer**, **Python 3.10 or newer**, **bubblewrap**, **iproute2**, and **coreutils**. Older distribution versions of mitmproxy may not support the required TUN API; the official standalone Linux `mitmdump` binary works. Choose its full path in Settings, or leave the setting empty to search `PATH`.
2. Configure your usual Wine runner or `umu-run`, Wine prefix, and Proton location in the launcher. Enable and configure the proxy separately on each account, as above.
3. Launch the account. The first proxied launch copies the selected Wine prefix into the launcher's `proxy-prefixes` data directory. This preserves its installed dependencies and gives each account its own wineserver, registry and profile. The copy uses filesystem copy-on-write when available and otherwise needs enough space for a full copy. Later launches reuse that account's copy. Changes to the original prefix after this first copy are not automatically synchronized.

Linux uses unprivileged user/network/PID namespaces through bubblewrap and `/dev/net/tun`. These must be permitted by the kernel and the distribution's security policy. The launcher does not require root or alter the host firewall, resolver, network interfaces or routing table. If namespace creation or routing fails, the game does not start. The launcher reports the failure instead of silently connecting directly.

Each account's Wine instance and its child processes run in a different network namespace with only loopback and a mitmproxy TUN interface. Both IPv4 and IPv6 Internet traffic have to cross a private Unix socket broker and the chosen upstream proxy. There is no veth or physical interface providing a direct escape route. The game loses all Internet connectivity if the tunnel stops, and the supervisor terminates that namespace's processes. Other accounts keep their own connections. Closing the launcher UI leaves the supervisor running until its game helper exits.

Wine's system DNS is isolated too: a private resolver configuration sends queries through the account proxy to `1.1.1.1:53`, using DNS over TCP even for incoming UDP queries. This also works with TCP-only HTTP CONNECT proxies, provided the proxy permits connections to port 53. The host's resolver is used only to find the proxy endpoint itself when its address is a hostname. Using a literal proxy IP avoids that lookup. Game TCP/UDP connections to other DNS servers follow the same proxy/UDP rules as other traffic.

SOCKS5 carries TCP and UDP when the server supports UDP ASSOCIATE. HTTP/HTTPS CONNECT carries TCP and the proxied system DNS conversion; other UDP is blocked. Unsupported protocols and fragmented SOCKS5 datagrams are not sent directly. This isolates the launched Wine process tree, not the whole desktop: the launcher's own downloads, native Steam/Epic authentication helpers, and unrelated applications retain their normal connections. An external browser opened through a desktop service is not a child of the isolated Wine instance.

## Windows traffic and lifetime

The native host creates GW2 suspended. One shared mitmproxy local-capture service routes each registered game PID to its own endpoint. A captured probe from the host verifies that the driver has applied the PID filter before GW2 resumes. Older native helpers reject the new proxy launch operations before creating GW2. Proxy-enabled catalogs use version 4 so older launchers refuse to open them instead of silently ignoring the proxy setting; existing version 3 catalogs still load.

Game TCP bytes, including TLS, pass through unchanged. There is no TLS interception, certificate installation, system proxy modification, or traffic recording. HTTPS CONNECT also validates the upstream proxy's TLS certificate using the runtime's normal trust store. SOCKS5 supports username/password authentication and UDP ASSOCIATE. HTTP/HTTPS CONNECT carries TCP only: UDP from the captured process is blocked, never sent directly. SOCKS5 UDP fragments are dropped.

The launcher does not fall back to a direct connection when a tunnel fails. A refused or unauthenticated tunnel stops the affected game and reports a proxy error. The native host monitors the service and stops its game if the pipe closes or heartbeats stop. Each proxied game also belongs to a kill-on-close Windows job. Closing the launcher UI keeps the host and proxy running until the game exits. The shared service releases an account route only after its game process exits, and exits after all clients have closed.

This is process routing, not a system VPN or a kernel-enforced network kill switch. The watchdog reacts after a capture failure; it cannot guarantee zero packets during a driver crash. Only registered `Gw2-64.exe` processes are captured. Windows DNS service traffic, loopback, multicast, separate browser/CEF processes, launcher updates, external programs, and Steam/Epic helper authentication retain their existing routes. Avoid running another local-capture engine at the same time. Wine/Proton uses the separate Linux namespace implementation above.

## Verification

`GW2_BUILD_TESTS=ON` adds `proxy-settings` and (when Python is available) `proxy-relay` to CTest, plus `proxy-control` and `proxy-storage` on Windows. The control tests cover private pipe messages and serialization between service shutdown and a new launch. Storage tests use an isolated catalog and real Windows encryption to check password retention across reopening, blank edits, username edits, disabling/re-enabling, failed saves, replacement, explicit removal, and the next launch's private request. These tests require neither GW2 nor administrator privileges.

Linux also runs `proxy-linux` broker, DNS, prefix-isolation and failure tests. To exercise real namespaces and the actual mitmproxy TUN stack, run:

```sh
KX_TEST_MITMDUMP=/absolute/path/to/mitmdump python3 tests/proxy_linux.py -v
```

The optional `proxy-wine-probe.exe` CMake target is built on Windows. Set `KX_TEST_WINE` to a Wine executable or `umu-run`, `KX_TEST_WINE_PREFIX` to a configured template prefix, and `KX_TEST_WINE_PROBE` to that probe's Linux path to additionally test WinSock IPv4/IPv6, DNS, UDP, private input and Windows child processes. For `umu-run`, also set its usual `PROTONPATH`, `GAMEID=0`, and `PROTON_VERB=runinprefix`. These probes use local fake upstream proxies and never launch GW2 or contact a real game server.

The optional Windows integration probe exercises the actual capture driver with two suspended synthetic game clients, using a SOCKS5 route and an HTTP CONNECT route concurrently. It checks TCP routing, SOCKS5 UDP, HTTP UDP blocking, another account surviving route cleanup, and a rejected proxy stopping only its own client. It never starts GW2 or contacts a real game server. Windows may request elevation for the capture driver:

```powershell
cmake --build build/debug --target proxy-capture-probe
py tests/proxy_capture.py --probe build/debug/proxy-capture-probe.exe --mitmdump C:/path/to/mitmdump.exe
```

The probe executable is at the root of the build directory because test targets are declared before the launcher's runtime output directory.

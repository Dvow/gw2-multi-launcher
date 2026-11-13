"""The launcher's mitmdump addon: PID capture with opaque SOCKS5 / CONNECT tunnels.

Each native host supplies one bounded JSON record over a private named pipe.
There are no listening proxy ports, certificate changes, or traffic logs.
One capture engine routes every registered GW2 PID to its own account proxy.
"""
import asyncio
import base64
import contextlib
import ipaddress
import json
import os
import socket
import ssl
import struct
import sys

PROBE_ADDRESS = ("198.18.0.254", 9)
PROBE_REPLY = b"KXPROXY\n"
TIMEOUT = 15


class ProxyError(Exception):
    """Deliberately contains no endpoint, credentials, or payload."""


def validate(config):
    protocol = config.get("protocol")
    host = config.get("host")
    port = config.get("port")
    user = config.get("username", "")
    password = config.get("password", "")
    if protocol not in ("socks5", "http", "https"):
        raise ProxyError()
    if not isinstance(host, str) or not 1 <= len(host) <= 253:
        raise ProxyError()
    if any(ord(c) <= 32 or ord(c) >= 127 or c in "/\\@?#[]" for c in host):
        raise ProxyError()
    if ":" in host:
        try:
            ipaddress.IPv6Address(host)
        except ValueError:
            raise ProxyError() from None
    if type(port) is not int or not 1 <= port <= 65535:
        raise ProxyError()
    for value in (user, password):
        if not isinstance(value, str) or len(value.encode("utf-8")) > 255:
            raise ProxyError()
        if any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ProxyError()
    if ":" in user or (password and not user):
        raise ProxyError()
    return config


def address_bytes(address):
    host, port = address[:2]
    if not 0 <= port <= 65535:
        raise ProxyError()
    try:
        ip = ipaddress.ip_address(host)
        data = bytes([1 if ip.version == 4 else 4]) + ip.packed
    except ValueError:
        name = host.encode("idna")
        if not 1 <= len(name) <= 255:
            raise ProxyError() from None
        data = bytes([3, len(name)]) + name
    return data + struct.pack("!H", port)


async def read_address(reader):
    kind = (await reader.readexactly(1))[0]
    if kind in (1, 4):
        host = str(ipaddress.ip_address(await reader.readexactly(4 if kind == 1 else 16)))
    elif kind == 3:
        size = (await reader.readexactly(1))[0]
        if not size:
            raise ProxyError()
        host = (await reader.readexactly(size)).decode("ascii")
    else:
        raise ProxyError()
    return host, struct.unpack("!H", await reader.readexactly(2))[0]


async def open_proxy(config):
    tls = ssl.create_default_context() if config["protocol"] == "https" else None
    reader, writer = await asyncio.open_connection(config["host"], config["port"], ssl=tls,
                                                   limit=16384)
    try:
        if config["protocol"] == "socks5":
            method = 2 if config.get("username") else 0
            writer.write(bytes([5, 1, method]))
            await writer.drain()
            if await reader.readexactly(2) != bytes([5, method]):
                raise ProxyError()
            if method == 2:
                user, password = (config.get(k, "").encode("utf-8") for k in ("username", "password"))
                writer.write(bytes([1, len(user)]) + user + bytes([len(password)]) + password)
                await writer.drain()
                if await reader.readexactly(2) != b"\x01\x00":
                    raise ProxyError()
        return reader, writer
    except BaseException:
        writer.close()
        raise


async def socks_request(reader, writer, command, address):
    writer.write(bytes([5, command, 0]) + address_bytes(address))
    await writer.drain()
    if await reader.readexactly(3) != b"\x05\x00\x00":
        raise ProxyError()
    return await read_address(reader)


async def open_tunnel(config, destination):
    reader, writer = await open_proxy(config)
    try:
        if config["protocol"] == "socks5":
            await socks_request(reader, writer, 1, destination)
        else:
            host, port = destination[:2]
            authority = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
            request = f"CONNECT {authority} HTTP/1.1\r\nHost: {authority}\r\n"
            if config.get("username"):
                token = base64.b64encode((config["username"] + ":" + config.get("password", "")).encode()).decode()
                request += f"Proxy-Authorization: Basic {token}\r\n"
            writer.write((request + "\r\n").encode("ascii"))
            await writer.drain()
            response = await reader.readuntil(b"\r\n\r\n")
            status = response.split(b"\r\n", 1)[0].split(b" ")
            if len(response) > 16384 or len(status) < 2 or status[0] not in (b"HTTP/1.0", b"HTTP/1.1") or status[1] != b"200":
                raise ProxyError()
        return reader, writer
    except BaseException:
        writer.close()
        raise


async def copy_stream(reader, writer):
    while data := await reader.read(65536):
        writer.write(data)
        await writer.drain()
    # Preserve TCP half-close: the other direction may still have a response.
    with contextlib.suppress(OSError, AttributeError, NotImplementedError):
        writer.write_eof()


class DatagramQueue(asyncio.DatagramProtocol):
    def __init__(self):
        self.packets = asyncio.Queue(maxsize=128)

    def datagram_received(self, data, _address):
        if not self.packets.full():
            self.packets.put_nowait(data)

    def error_received(self, _error):
        if not self.packets.full():
            self.packets.put_nowait(None)


async def relay_udp(stream, config, destination):
    writer = None
    transport = None
    tasks = []
    try:
        async def connect():
            nonlocal writer, transport
            reader, writer = await open_proxy(config)
            relay = await socks_request(reader, writer, 3, ("0.0.0.0", 0))
            if relay[0] in ("0.0.0.0", "::"):
                relay = (writer.get_extra_info("peername")[0], relay[1])
            if not relay[1]:
                raise ProxyError()
            transport, queue = await asyncio.get_running_loop().create_datagram_endpoint(DatagramQueue, remote_addr=relay)
            return reader, queue

        # wait_for also supports the Python 3.10 used by the Linux build tests.
        reader, queue = await asyncio.wait_for(connect(), TIMEOUT)
        header = b"\0\0\0" + address_bytes(destination)

        async def upload():
            while data := await stream.read(65535):
                if len(header) + len(data) <= 65507:
                    transport.sendto(header + data)

        async def download():
            while True:
                data = await queue.packets.get()
                if data is None:
                    return
                if len(data) < 4 or data[:3] != b"\0\0\0":
                    continue  # Fragmented SOCKS datagrams are unsupported; never send directly.
                packet = asyncio.StreamReader()
                packet.feed_data(data[3:])
                packet.feed_eof()
                try:
                    source = await read_address(packet)
                except (ProxyError, UnicodeError, asyncio.IncompleteReadError):
                    continue
                if source != destination:
                    continue
                stream.write(await packet.read())
                await stream.drain()

        tasks = [asyncio.create_task(upload()), asyncio.create_task(download()), asyncio.create_task(reader.read(1))]
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if transport:
            transport.close()
        if writer:
            writer.close()


class Route:
    def __init__(self, connection, request):
        self.connection = connection
        self.pid, self.owner = request["pid"], request["owner"]
        if any(type(pid) is not int or not 0 < pid < 2**32 for pid in (self.pid, self.owner)):
            raise ProxyError()
        if self.pid == self.owner or self.owner != connection.owner:
            raise ProxyError()
        self.config = validate(request["proxy"])
        self.game = connection.game_handle(self.pid)
        try:
            self.owner_process = connection.game_handle(self.owner)
        except Exception:
            connection.server.kernel.CloseHandle(self.game)
            raise
        self.ready = self.failed = False
        self.tasks = set()

    def report(self, status):
        self.connection.report(status)

    def fail(self, code=1021):
        if not self.failed:
            self.failed = True
            self.report(f"ERROR {code}")

    def close(self):
        for task in self.tasks:
            task.cancel()
        self.connection.server.kernel.CloseHandle(self.game)
        self.connection.server.kernel.CloseHandle(self.owner_process)
        self.connection.close()


class Capture:
    def __init__(self):
        self.redirector = None
        self.routes = {}
        self.owners = {}
        self.pending = set()
        self.start_lock = asyncio.Lock()
        self.idle_since = None
        self.broken = False

    def load(self, loader):
        loader.add_option("kx_proxy_pipe", str, "", "Private proxy control pipe.")

    def spec(self):
        return ",".join(str(pid) for pid in (*self.routes, *self.owners)) or "0"

    async def tcp(self, stream):
        writer = None
        tasks = []
        route = None
        try:
            pid = stream.get_extra_info("pid")
            destination = stream.get_extra_info("remote_endpoint", stream.get_extra_info("sockname"))[:2]
            if pid in self.owners and destination == PROBE_ADDRESS:
                route = self.owners[pid]
                if not route.failed:
                    stream.write(PROBE_REPLY)
                    await stream.drain()
                    route.ready = True
                    route.report("READY")
                return
            route = self.routes.get(pid)
            if route is None or route.failed or not route.ready:
                return
            route.tasks.add(asyncio.current_task())
            reader, writer = await asyncio.wait_for(open_tunnel(route.config, destination), TIMEOUT)
            tasks = [asyncio.create_task(copy_stream(stream, writer)), asyncio.create_task(copy_stream(reader, stream))]
            await asyncio.gather(*tasks)
        except asyncio.CancelledError:
            raise
        except Exception:
            # Connection refusal, authentication failure, or invalid tunnel reply
            # stops only its account. Ordinary resets after setup close this stream.
            if route is not None and writer is None:
                route.fail()
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            if writer:
                writer.close()
            stream.close()
            if route:
                route.tasks.discard(asyncio.current_task())

    async def udp(self, stream):
        route = self.routes.get(stream.get_extra_info("pid"))
        try:
            if route is None or route.failed or not route.ready or route.config["protocol"] != "socks5":
                return  # CONNECT cannot carry UDP; never bypass the selected proxy.
            route.tasks.add(asyncio.current_task())
            destination = stream.get_extra_info("remote_endpoint", stream.get_extra_info("sockname"))[:2]
            await relay_udp(stream, route.config, destination)
        except asyncio.CancelledError:
            raise
        except Exception:
            route.fail()
        finally:
            stream.close()
            if route:
                route.tasks.discard(asyncio.current_task())

    def accept(self, connection, record):
        if connection is None:
            self.broken = True
            return
        task = asyncio.create_task(self.register(connection, record))
        self.pending.add(task)
        task.add_done_callback(self.pending.discard)

    async def register(self, connection, record):
        route = None
        try:
            request = json.loads(record)
            route = Route(connection, request)
            del record, request
            async with self.start_lock:
                if self.broken or route.pid in self.routes or route.owner in self.owners:
                    raise ProxyError()
                if self.redirector is None:
                    import mitmproxy_rs
                    self.redirector = await mitmproxy_rs.local.start_local_redirector(self.tcp, self.udp)
                    self.watch = asyncio.ensure_future(self.redirector.wait_closed())
                if connection.disconnected.is_set() or connection.exited(route.game):
                    raise ProxyError()
                self.routes[route.pid] = route
                self.owners[route.owner] = route
                self.redirector.set_intercept(self.spec())
                route.report("PROBE")
                self.idle_since = None
        except Exception:
            connection.report("ERROR 1020")
            if route:
                if self.routes.get(route.pid) is route:
                    del self.routes[route.pid]
                    self.owners.pop(route.owner, None)
                route.close()
            else:
                connection.close()

    def running(self):
        self.main = asyncio.create_task(self.run())

    async def run(self):
        from mitmproxy import ctx
        server = None
        try:
            if os.name != "nt" or not ctx.options.kx_proxy_pipe.startswith("\\\\.\\pipe\\KXProxy-"):
                raise ProxyError()
            # mitmdump's addon importer does not promise to retain this path.
            from pathlib import Path
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from kx_proxy_pipe import WindowsPipes
            server = WindowsPipes(ctx.options.kx_proxy_pipe, asyncio.get_running_loop(), self.accept)
            self.idle_since = asyncio.get_running_loop().time()
            while True:
                if self.redirector and self.watch.done():
                    self.broken = True
                for pid, route in list(self.routes.items()):
                    if route.connection.exited(route.game):
                        del self.routes[pid]
                        self.owners.pop(route.owner, None)
                        route.close()
                        if not self.broken:
                            self.redirector.set_intercept(self.spec())
                    elif self.broken:
                        route.fail(1022)
                    else:
                        route.report("ALIVE")
                    # A host dying closes its kill-on-close job. Keep the route
                    # until the process handle confirms that GW2 actually exited.
                if not self.routes and not self.pending:
                    now = asyncio.get_running_loop().time()
                    if self.idle_since is None:
                        self.idle_since = now
                    if now - self.idle_since > 10 and server.begin_shutdown():
                        break
                await asyncio.sleep(1)
        except Exception:
            for route in self.routes.values():
                route.fail(1022)
            # Allow native hosts to stop their games before releasing capture.
            while any(not r.connection.exited(r.game) for r in self.routes.values()):
                await asyncio.sleep(0.1)
        finally:
            remaining = [*self.pending, *(task for route in self.routes.values() for task in route.tasks)]
            for task in remaining:
                task.cancel()
            for route in self.routes.values():
                route.close()
            await asyncio.gather(*remaining, return_exceptions=True)
            if self.redirector:
                self.redirector.close()
                with contextlib.suppress(Exception):
                    await self.watch
            if server:
                server.close()
            ctx.master.shutdown()


addons = [Capture()]



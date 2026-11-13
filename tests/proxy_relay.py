"""Loopback protocol and routing tests. No driver, game, or internet required."""
import asyncio
import contextlib
import pathlib
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "proxy"))
import kx_proxy as proxy


class RelayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.servers = []
        self.tasks = set()
        self.errors = []

    async def asyncTearDown(self):
        for server in self.servers:
            server.close()
            await server.wait_closed()
        for task in self.tasks:
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        if self.errors:
            raise self.errors[0]

    async def server(self, handler):
        async def tracked(reader, writer):
            task = asyncio.current_task()
            self.tasks.add(task)
            try:
                await handler(reader, writer)
            except (ConnectionError, asyncio.IncompleteReadError):
                pass
            except Exception as error:
                self.errors.append(error)
            finally:
                writer.close()
                with contextlib.suppress(ConnectionError):
                    await writer.wait_closed()
                self.tasks.discard(task)
        server = await asyncio.start_server(tracked, "127.0.0.1", 0)
        self.servers.append(server)
        return server.sockets[0].getsockname()[1]

    def config(self, port, protocol="socks5", user="", password=""):
        return proxy.validate(dict(protocol=protocol, host="127.0.0.1", port=port, username=user, password=password))

    async def socks(self, reader, writer, user=b"", password=b"", reject=False):
        method = 2 if user else 0
        self.assertEqual(await reader.readexactly(3), bytes([5, 1, method]))
        writer.write(bytes([5, method]))
        await writer.drain()
        if method == 2:
            self.assertEqual(await reader.readexactly(1), b"\x01")
            size = (await reader.readexactly(1))[0]
            self.assertEqual(await reader.readexactly(size), user)
            size = (await reader.readexactly(1))[0]
            self.assertEqual(await reader.readexactly(size), password)
            writer.write(b"\x01\x01" if reject else b"\x01\x00")
            await writer.drain()
            if reject:
                return
        self.assertEqual(await reader.readexactly(3), b"\x05\x01\x00")
        destination = await proxy.read_address(reader)
        self.assertEqual(destination, ("203.0.113.7", 6112))
        # Fragment the handshake to exercise stream framing.
        for part in (b"\x05\x00", b"\x00\x01\0\0", b"\0\0\0\0"):
            writer.write(part)
            await writer.drain()
        return destination

    async def test_socks_authentication_and_binary_payload(self):
        payload = b"\x16\x03\x01\x00\xffGW2\0opaque-TLS"
        async def upstream(reader, writer):
            await self.socks(reader, writer, b"account-A", b"secret-A")
            self.assertEqual(await reader.readexactly(len(payload)), payload)
            writer.write(payload[::-1])
            await writer.drain()
        port = await self.server(upstream)
        reader, writer = await proxy.open_tunnel(self.config(port, user="account-A", password="secret-A"), ("203.0.113.7", 6112))
        writer.write(payload)
        await writer.drain()
        self.assertEqual(await reader.readexactly(len(payload)), payload[::-1])
        writer.close()

    async def test_socks_bad_credentials_do_not_fall_back(self):
        port = await self.server(lambda r, w: self.socks(r, w, b"user", b"wrong", True))
        with self.assertRaises(proxy.ProxyError):
            await proxy.open_tunnel(self.config(port, user="user", password="wrong"), ("203.0.113.7", 6112))

    async def test_socks_rejects_authentication_downgrade(self):
        async def upstream(reader, writer):
            await reader.readexactly(3)
            writer.write(b"\x05\x00")
            await writer.drain()
        port = await self.server(upstream)
        with self.assertRaises(proxy.ProxyError):
            await proxy.open_tunnel(self.config(port, user="user", password="pass"), ("203.0.113.7", 6112))

    async def test_connect_preserves_bytes_after_headers(self):
        async def upstream(reader, writer):
            header = await reader.readuntil(b"\r\n\r\n")
            self.assertTrue(header.startswith(b"CONNECT [2001:db8::1]:6112 HTTP/1.1\r\n"))
            self.assertIn(b"Proxy-Authorization: Basic dXNlcjpwYXNz\r\n", header)
            writer.write(b"HTTP/1.1 200 Connection established\r\n\r\nserver-first\0\xff")
            await writer.drain()
        port = await self.server(upstream)
        reader, writer = await proxy.open_tunnel(self.config(port, "http", "user", "pass"), ("2001:db8::1", 6112))
        self.assertEqual(await reader.read(), b"server-first\0\xff")
        writer.close()

    async def test_connect_refusal(self):
        async def upstream(reader, writer):
            await reader.readuntil(b"\r\n\r\n")
            writer.write(b"HTTP/1.1 407 Proxy Authentication Required\r\n\r\n")
            await writer.drain()
        port = await self.server(upstream)
        with self.assertRaises(proxy.ProxyError):
            await proxy.open_tunnel(self.config(port, "http"), ("203.0.113.7", 6112))

    async def test_https_never_downgrades_to_plain_http(self):
        async def upstream(reader, writer):
            self.assertEqual(await reader.readexactly(1), b"\x16")  # TLS ClientHello
            writer.write(b"HTTP/1.1 200 OK\r\n\r\n")
            await writer.drain()
        port = await self.server(upstream)
        with self.assertRaises(OSError):
            await proxy.open_tunnel(self.config(port, "https"), ("203.0.113.7", 6112))

    async def test_connect_oversized_response(self):
        async def upstream(reader, writer):
            await reader.readuntil(b"\r\n\r\n")
            writer.write(b"HTTP/1.1 200 OK\r\nX: " + b"x" * 20000 + b"\r\n\r\n")
            await writer.drain()
        port = await self.server(upstream)
        with self.assertRaises((proxy.ProxyError, asyncio.LimitOverrunError)):
            await proxy.open_tunnel(self.config(port, "http"), ("203.0.113.7", 6112))

    async def test_two_pids_use_different_proxies(self):
        class Route:
            ready, failed = True, False
            def __init__(self, config):
                self.config, self.tasks = config, set()
            def fail(self):
                self.failed = True
        async def upstream(tag, reader, writer):
            await self.socks(reader, writer)
            request = await reader.read()  # Reply only after TCP half-close.
            writer.write(tag + request)
            await writer.drain()
        ports = [await self.server(lambda r, w: upstream(b"A:", r, w)),
                 await self.server(lambda r, w: upstream(b"B:", r, w))]
        capture = proxy.Capture()
        capture.routes = {11: Route(self.config(ports[0])), 22: Route(self.config(ports[1]))}

        class Stream:
            def __init__(self, pid, reader, writer):
                self.pid, self.reader, self.writer = pid, reader, writer
            def get_extra_info(self, name, default=None):
                return {"pid": self.pid, "remote_endpoint": ("203.0.113.7", 6112)}.get(name, default)
            async def read(self, count): return await self.reader.read(count)
            def write(self, data): self.writer.write(data)
            async def drain(self): await self.writer.drain()
            def write_eof(self): self.writer.write_eof()
            def close(self): self.writer.close()

        async def client(pid):
            port = await self.server(lambda r, w: capture.tcp(Stream(pid, r, w)))
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(b"game")
            await writer.drain()
            writer.write_eof()
            try:
                response = await asyncio.wait_for(reader.read(), 3)
            except ConnectionResetError:
                if pid != 33:
                    raise
                response = b""
            writer.close()
            return response
        self.assertEqual(await asyncio.gather(client(11), client(22), client(33)), [b"A:game", b"B:game", b""])
        self.assertFalse(any(route.failed for route in capture.routes.values()))

    async def test_http_udp_is_blocked(self):
        class Stream:
            closed = False
            def get_extra_info(self, _name): return 11
            def close(self): self.closed = True
        class Route:
            ready, failed = True, False
            config, tasks = {"protocol": "http"}, set()
        capture = proxy.Capture()
        capture.routes[11] = Route()
        stream = Stream()
        await capture.udp(stream)
        self.assertTrue(stream.closed)

    async def test_udp_association_timeout_closes_control_connection(self):
        requested = asyncio.Event()
        closed = asyncio.Event()

        async def upstream(reader, writer):
            self.assertEqual(await reader.readexactly(3), b"\x05\x01\x00")
            writer.write(b"\x05\x00")
            await writer.drain()
            self.assertEqual(await reader.readexactly(3), b"\x05\x03\x00")
            self.assertEqual(await proxy.read_address(reader), ("0.0.0.0", 0))
            requested.set()
            # Leave UDP ASSOCIATE unanswered. Timeout must close the TCP control socket.
            self.assertEqual(await reader.read(), b"")
            closed.set()

        port = await self.server(upstream)
        with patch.object(proxy, "TIMEOUT", 1):
            with self.assertRaises(asyncio.TimeoutError):
                await proxy.relay_udp(object(), self.config(port), ("203.0.113.7", 6112))
        self.assertTrue(requested.is_set())
        await asyncio.wait_for(closed.wait(), 3)

    async def test_socks_udp_payload_round_trip(self):
        received = asyncio.get_running_loop().create_future()
        response = asyncio.get_running_loop().create_future()
        payload = b"game\0\xffUDP"
        destination = ("203.0.113.7", 6112)
        packet = b"\0\0\0" + proxy.address_bytes(destination) + payload

        class Echo(asyncio.DatagramProtocol):
            def connection_made(self, transport): self.transport = transport
            def datagram_received(self, data, address):
                if not received.done():
                    received.set_result(data)
                self.transport.sendto(data, address)

        udp, _ = await asyncio.get_running_loop().create_datagram_endpoint(Echo, local_addr=("127.0.0.1", 0))
        self.addCleanup(udp.close)

        async def upstream(reader, writer):
            self.assertEqual(await reader.readexactly(3), b"\x05\x01\x00")
            writer.write(b"\x05\x00")
            await writer.drain()
            self.assertEqual(await reader.readexactly(3), b"\x05\x03\x00")
            self.assertEqual(await proxy.read_address(reader), ("0.0.0.0", 0))
            writer.write(b"\x05\x00\x00" + proxy.address_bytes(udp.get_extra_info("sockname")))
            await writer.drain()
            await reader.read()

        incoming = asyncio.Queue()
        incoming.put_nowait(payload)

        class Stream:
            async def read(self, _count): return await incoming.get()
            def write(self, data): response.set_result(data)
            async def drain(self): pass

        port = await self.server(upstream)
        relay = asyncio.create_task(proxy.relay_udp(Stream(), self.config(port), destination))
        try:
            self.assertEqual(await asyncio.wait_for(response, 3), payload)
            self.assertEqual(received.result(), packet)
        finally:
            relay.cancel()
            await asyncio.gather(relay, return_exceptions=True)

    async def test_address_encodings(self):
        for address in (("1.2.3.4", 6112), ("2001:db8::1", 65535), ("proxy.example.com", 443)):
            reader = asyncio.StreamReader()
            reader.feed_data(proxy.address_bytes(address))
            reader.feed_eof()
            self.assertEqual(await proxy.read_address(reader), address)

    async def test_validation(self):
        base = self.config(1080)
        for field, value in (("host", "x\r\nInject"), ("host", "http://host"), ("port", 65536),
                             ("port", True), ("username", "a:b"), ("password", "p"), ("protocol", "socks4")):
            with self.assertRaises(proxy.ProxyError):
                proxy.validate({**base, field: value})


if __name__ == "__main__":
    unittest.main()

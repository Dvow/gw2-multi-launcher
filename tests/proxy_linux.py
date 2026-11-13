"""Linux broker tests; set KX_TEST_MITMDUMP to include real TUN/namespace tests."""
import asyncio
import contextlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "proxy"))
import kx_proxy as relay
import kx_proxy_linux as linux


class LinuxTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kx-proxy-test-")
        self.directory = Path(self.temp.name)
        self.servers, self.brokers, self.tasks, self.children = [], [], set(), []
        self.errors, self.visits = [], []
        self.udp = None

    async def asyncTearDown(self):
        for process in self.children:
            if process.returncode is None:
                process.kill()
                await process.wait()
        for broker in self.brokers:
            await broker.close()
        for server in self.servers:
            server.close()
            await server.wait_closed()
        for task in list(self.tasks):
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        if self.udp:
            self.udp.close()
        self.temp.cleanup()
        if self.errors:
            raise self.errors[0]

    async def upstream(self, tag, protocol="socks5", reject=False):
        async def handler(reader, writer):
            task = asyncio.current_task()
            self.tasks.add(task)
            try:
                if protocol == "socks5":
                    self.assertEqual(await reader.readexactly(3), b"\x05\x01\x00")
                    writer.write(b"\x05\xff" if reject else b"\x05\x00")
                    await writer.drain()
                    if reject:
                        return
                    command = await reader.readexactly(3)
                    destination = await relay.read_address(reader)
                    if command == b"\x05\x03\x00":
                        self.assertEqual(destination, ("0.0.0.0", 0))
                        writer.write(b"\x05\x00\x00" + relay.address_bytes(self.udp.get_extra_info("sockname")))
                        await writer.drain()
                        await reader.read()
                        return
                    self.assertEqual(command, b"\x05\x01\x00")
                    writer.write(b"\x05\x00\x00\x01\0\0\0\0\0\0")
                else:
                    header = await reader.readuntil(b"\r\n\r\n")
                    authority = header.split(b" ")[1].decode()
                    host, port = authority.rsplit(":", 1)
                    destination = (host.strip("[]"), int(port))
                    writer.write(b"HTTP/1.1 407 Refused\r\n\r\n" if reject else b"HTTP/1.1 200 OK\r\n\r\n")
                    if reject:
                        await writer.drain()
                        return
                self.visits.append((tag, destination))
                await writer.drain()
                if destination == linux.DNS_UPSTREAM:
                    size = struct.unpack("!H", await reader.readexactly(2))[0]
                    query = await reader.readexactly(size)
                    # Answer one A question, preserving transaction ID/question.
                    answer = query[:2] + b"\x81\x80\x00\x01\x00\x01\x00\x00\x00\x00" + query[12:]
                    answer += b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04\xcb\x00\x71\x07"
                    writer.write(struct.pack("!H", len(answer)) + answer)
                    await writer.drain()
                else:
                    while payload := await reader.read(65535):
                        writer.write(tag.encode() + b":" + payload)
                        await writer.drain()
            except (ConnectionError, asyncio.IncompleteReadError):
                pass
            except Exception as error:
                self.errors.append(error)
            finally:
                writer.close()
                self.tasks.discard(task)
        server = await asyncio.start_server(handler, "127.0.0.1", 0)
        self.servers.append(server)
        return dict(protocol=protocol, host="127.0.0.1", port=server.sockets[0].getsockname()[1])

    async def broker(self, config):
        instance = linux.Broker(config)
        instance.ready.set()
        path = self.directory / str(len(self.brokers))
        server = await asyncio.start_unix_server(instance.accept, path=str(path), limit=8192)
        self.servers.append(server)
        self.brokers.append(instance)
        return instance, path

    async def connect(self, path, op="tcp", destination=("203.0.113.7", 6112)):
        reader, writer = await asyncio.open_unix_connection(str(path))
        writer.write(json.dumps(dict(op=op, host=destination[0], port=destination[1])).encode() + b"\n")
        await writer.drain()
        return reader, writer

    async def test_accounts_have_independent_routes_and_failures(self):
        a, ap = await self.broker(await self.upstream("A"))
        b, bp = await self.broker(await self.upstream("B", "http"))
        for path, marker in ((ap, b"A"), (bp, b"B")):
            reader, writer = await self.connect(path)
            self.assertEqual(await reader.readline(), b"OK\n")
            writer.write(b"\x00opaque\xff")
            writer.write_eof()
            self.assertEqual(await reader.read(), marker + b":\x00opaque\xff")
            writer.close()
        a.failed.set()
        reader, writer = await self.connect(bp)
        self.assertEqual(await reader.readline(), b"OK\n")
        writer.close()
        self.assertFalse(b.failed.is_set())

    async def test_http_udp_is_blocked_without_upstream_connection(self):
        broker, path = await self.broker(await self.upstream("B", "http"))
        reader, writer = await self.connect(path, "udp")
        self.assertEqual(await reader.read(), b"")
        self.assertEqual(self.visits, [])
        self.assertFalse(broker.failed.is_set())
        writer.close()

    async def test_refused_proxy_fails_account_without_direct_retry(self):
        broker, path = await self.broker(await self.upstream("A", reject=True))
        reader, writer = await self.connect(path)
        self.assertEqual(await reader.read(), b"")
        self.assertTrue(broker.failed.is_set())
        self.assertEqual(self.visits, [])
        writer.close()

    async def test_dns_udp_and_tcp_use_selected_connect_proxy(self):
        broker, path = await self.broker(await self.upstream("B", "http"))
        query = b"\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x04test\x07invalid\x00\x00\x01\x00\x01"
        for op, framing in (("udp", "!I"), ("tcp", "!H")):
            reader, writer = await self.connect(path, op, (linux.DNS_ADDRESS, 53))
            self.assertEqual(await reader.readline(), b"OK\n")
            writer.write(struct.pack(framing, len(query)) + query)
            await writer.drain()
            size = struct.unpack(framing, await reader.readexactly(struct.calcsize(framing)))[0]
            answer = await reader.readexactly(size)
            self.assertEqual(answer[:2], query[:2])
            self.assertEqual(answer[-4:], b"\xcb\x00\x71\x07")
            writer.close()
        self.assertEqual(self.visits, [("B", linux.DNS_UPSTREAM)] * 2)
        self.assertFalse(broker.failed.is_set())

    async def test_prefixes_are_copied_not_shared_and_locked(self):
        base = self.directory / "base"
        base.mkdir()
        (base / "system.reg").write_text("original")
        (base / "pfx").symlink_to(base)
        config = {"state": str(self.directory / "state"), "account": "a"}
        first, lock1 = linux.prepare_prefix(config, base)
        try:
            with self.assertRaises(BlockingIOError):
                linux.prepare_prefix(config, base)
            config["account"] = "b"
            second, lock2 = linux.prepare_prefix(config, base)
            try:
                (first / "system.reg").write_text("changed")
                self.assertEqual((second / "system.reg").read_text(), "original")
                self.assertEqual((base / "system.reg").read_text(), "original")
                self.assertNotEqual(first.stat().st_ino, second.stat().st_ino)
                self.assertEqual((first / "pfx").resolve(), first)
            finally:
                os.close(lock2)
        finally:
            os.close(lock1)

    async def udp_echo(self):
        class Echo(asyncio.DatagramProtocol):
            def connection_made(self, transport):
                self.transport = transport
            def datagram_received(self, data, address):
                self.transport.sendto(data, address)
        self.udp, _ = await asyncio.get_running_loop().create_datagram_endpoint(Echo, local_addr=("127.0.0.1", 0))

    async def start(self, config, tag, mode="normal"):
        base = Path(os.environ["KX_TEST_WINE_PREFIX"]) if mode == "wine" else self.directory / "base"
        if mode != "wine":
            base.mkdir(exist_ok=True)
            (base / "system.reg").write_text("test registry")
        config.update(account=tag, state=str(self.directory / "state"), mitmdump=os.environ["KX_TEST_MITMDUMP"])
        fifo = self.directory / (tag + "-input-" + str(len(self.children)))
        os.mkfifo(fifo, 0o600)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        command = [os.environ["KX_TEST_WINE"], os.environ["KX_TEST_WINE_PROBE"], tag, config["protocol"]] if mode == "wine" else [
            sys.executable, str(Path(__file__).resolve()), "--probe", tag, config["protocol"], mode]
        process = await asyncio.create_subprocess_exec(sys.executable, str(ROOT / "proxy/kx_proxy_linux.py"),
            "--", *command, "--input", str(fifo),
            env=dict(os.environ, WINEPREFIX=str(base)), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        self.children.append(process)
        raw = json.dumps(config).encode()
        os.write(fd, struct.pack("<I", len(raw)) + raw + b"secret-through-fifo")
        return process, fd

    async def launch(self, config, tag):
        process, fd = await self.start(config, tag)
        try:
            output, error = await asyncio.wait_for(process.communicate(), 70)
            return process.returncode, output, error
        finally:
            os.close(fd)

    @unittest.skipUnless(os.environ.get("KX_TEST_MITMDUMP"), "set KX_TEST_MITMDUMP for real network namespace tests")
    async def test_real_tun_two_accounts_ipv4_ipv6_dns_udp_and_children(self):
        await self.udp_echo()
        a, b = await self.upstream("A"), await self.upstream("B", "http")
        results = await asyncio.gather(self.launch(a, "A"), self.launch(b, "B"))
        for code, output, error in results:
            self.assertEqual(code, 0, (output, error))
            self.assertIn(b"PROBE OK", output)
        self.assertIn(("A", ("2001:db8::7", 6112)), self.visits)
        self.assertIn(("B", linux.DNS_UPSTREAM), self.visits)

    @unittest.skipUnless(os.environ.get("KX_TEST_MITMDUMP"), "set KX_TEST_MITMDUMP for real network namespace tests")
    async def test_real_proxy_rejection_stops_only_affected_namespace(self):
        a, b = await self.upstream("A", reject=True), await self.upstream("B", "http")
        failed, good = await asyncio.gather(self.launch(a, "A"), self.launch(b, "B"))
        self.assertEqual(failed[0], 1, failed)
        self.assertIn(struct.pack("<III", linux.MAGIC, 6, 1021), failed[1])
        self.assertEqual(good[0], 0, good)
        self.assertIn(b"PROBE OK", good[1])
        self.assertTrue(all(tag == "B" for tag, _ in self.visits))

    @unittest.skipUnless(os.environ.get("KX_TEST_MITMDUMP"), "set KX_TEST_MITMDUMP for real network namespace tests")
    async def test_broker_crash_kills_namespace_and_its_children(self):
        process, fd = await self.start(await self.upstream("B", "http"), "B", "hold")
        try:
            await asyncio.wait_for(process.stdout.readuntil(b"HOLD\n"), 30)
            descendants = set()
            pending = [process.pid]
            while pending:
                pid = pending.pop()
                children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
                for child in children:
                    descendants.add(int(child))
                    pending.append(int(child))
            self.assertGreaterEqual(len(descendants), 3)
            process.kill()
            await process.wait()
            for _ in range(100):
                live = []
                for pid in descendants:
                    try:
                        if Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].strip()[0] != "Z":
                            live.append(pid)
                    # Gone, or reaped between opening and reading its stat (ESRCH).
                    except (FileNotFoundError, ProcessLookupError):
                        pass
                if not live:
                    break
                await asyncio.sleep(.05)
            self.assertEqual(live, [])
        finally:
            os.close(fd)

    @unittest.skipUnless(os.environ.get("KX_TEST_MITMDUMP"), "set KX_TEST_MITMDUMP for real network namespace tests")
    async def test_closing_launcher_output_keeps_proxy_alive(self):
        process, fd = await self.start(await self.upstream("B", "http"), "B", "detach")
        try:
            await asyncio.wait_for(process.stdout.readuntil(b"DETACH\n"), 30)
            process.stdout._transport.close()  # The launcher's read end disappears.
            self.assertEqual(await asyncio.wait_for(process.wait(), 20), 0)
            markers = list((self.directory / "state").glob("*/prefix/survived"))
            self.assertEqual(len(markers), 1)
            self.assertEqual(markers[0].read_bytes(), b"B:after-close")
        finally:
            os.close(fd)

    @unittest.skipUnless(os.environ.get("KX_TEST_MITMDUMP"), "set KX_TEST_MITMDUMP for real network namespace tests")
    async def test_failed_capture_start_never_launches_directly(self):
        for magic in (linux.MAGIC, 0xA31C7E5D):
            with self.subTest(magic=magic):
                config = dict(await self.upstream("B", "http"), magic=magic)
                with patch.dict(os.environ, KX_TEST_MITMDUMP="/bin/false"):
                    process, fd = await self.start(config, "B")
                try:
                    output, error = await asyncio.wait_for(process.communicate(), 15)
                    self.assertEqual(process.returncode, 1, error)
                    self.assertIn(struct.pack("<III", magic, 14, 0), output)
                    self.assertIn(struct.pack("<III", magic, 6, 1024), output)
                    self.assertTrue(output.endswith(struct.pack("<III", magic, 5, 0)))
                    self.assertNotIn(b"PROBE OK", output)
                    self.assertEqual(self.visits, [])
                finally:
                    os.close(fd)

    @unittest.skipUnless(os.environ.get("KX_TEST_WINE"), "set KX_TEST_WINE, KX_TEST_WINE_PREFIX and KX_TEST_WINE_PROBE")
    async def test_real_wine_winsock_dns_and_child_process(self):
        await self.udp_echo()
        processes = []
        try:
            for tag, protocol in (("A", "socks5"), ("B", "http")):
                processes.append(await self.start(await self.upstream(tag, protocol), tag, "wine"))
            for process, _ in processes:
                output, error = await asyncio.wait_for(process.communicate(), 90)
                self.assertEqual(process.returncode, 0, (output, error))
                self.assertIn(b"PROBE OK", output)
        finally:
            for _, fd in processes:
                os.close(fd)


def probe():
    tag, protocol = sys.argv[2:4]
    with open(sys.argv[-1], "rb", buffering=0) as fifo:
        assert fifo.read(19) == b"secret-through-fifo"
    assert {name for _, name in socket.if_nameindex()} == {"lo", "kxtun0"}
    status = Path("/proc/self/status").read_text()
    assert "CapEff:\t0000000000000000" in status, status
    if sys.argv[4] == "hold":
        subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"])
        print("HOLD", flush=True)
        import time
        time.sleep(120)
        return
    if sys.argv[4] == "detach":
        print("DETACH", flush=True)
        import time
        time.sleep(.5)
        with socket.create_connection(("203.0.113.7", 6112), 10) as stream:
            stream.sendall(b"after-close")
            reply = stream.recv(1024)
        Path(os.environ["WINEPREFIX"], "survived").write_bytes(reply)
        print("DONE", flush=True)
        return
    for address, family in (("203.0.113.7", socket.AF_INET), ("2001:db8::7", socket.AF_INET6)):
        with socket.socket(family, socket.SOCK_STREAM) as stream:
            stream.settimeout(10)
            stream.connect((address, 6112))
            stream.sendall(b"payload")
            assert stream.recv(1024) == tag.encode() + b":payload"
    assert socket.gethostbyname("test.invalid") == "203.0.113.7"
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stream:
        stream.settimeout(2)
        stream.sendto(b"datagram-one", ("203.0.113.7", 6112))
        if protocol == "socks5":
            assert stream.recv(1024) == b"datagram-one"
            stream.sendto(b"second", ("203.0.113.7", 6112))
            assert stream.recv(1024) == b"second"
        else:
            try:
                stream.recv(1024)
                raise AssertionError("HTTP UDP escaped")
            except TimeoutError:
                pass
    # A separate child inherits the same network namespace.
    script = "import socket; s=socket.create_connection(('203.0.113.7',6112),10); s.sendall(b'child'); print(s.recv(99).decode())"
    output = subprocess.check_output([sys.executable, "-c", script])
    assert output.strip() == tag.encode() + b":child"
    print("PROBE OK", flush=True)


if __name__ == "__main__":
    if "--probe" in sys.argv:
        probe()
    else:
        unittest.main()

"""Linux per-account supervisor. All Internet sockets belong to this broker.

Wine and mitmproxy's TUN stack run inside a private network/PID namespace.
Only opaque TCP and framed UDP cross a private Unix socket to this process.
There is no veth, host route, privileged installation, or direct fallback.
"""
import asyncio
import contextlib
import fcntl
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import threading

from kx_proxy import ProxyError, copy_stream, open_tunnel, relay_udp, validate

MAGIC = 0x374C4D47
DNS_ADDRESS = "198.18.0.53"
DNS_UPSTREAM = ("1.1.1.1", 53)
MAX_RECORD = 8192


def read_exact(fd, length):
    data = bytearray()
    while len(data) < length:
        chunk = os.read(fd, length - len(data))
        if not chunk:
            raise ProxyError()
        data.extend(chunk)
    return data


def report(code, state=6):
    with contextlib.suppress(BrokenPipeError):
        os.write(1, struct.pack("<III", MAGIC, state, code))
        if state == 6:
            os.write(1, struct.pack("<III", MAGIC, 5, 0))


class DatagramStream:
    """Length framing preserves UDP packet boundaries on the Unix connection."""
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer

    async def read(self, _size):
        try:
            size = struct.unpack("!I", await self.reader.readexactly(4))[0]
        except asyncio.IncompleteReadError:
            return b""
        if not 0 < size <= 65535:
            raise ProxyError()
        return await self.reader.readexactly(size)

    def write(self, data):
        self.writer.write(struct.pack("!I", len(data)) + data)

    async def drain(self):
        await self.writer.drain()


async def dns_query(config, query):
    """Resolve inside the selected proxy, including with TCP-only CONNECT."""
    if not 12 <= len(query) <= 65535:
        raise ProxyError()
    reader, writer = await open_tunnel(config, DNS_UPSTREAM)
    try:
        writer.write(struct.pack("!H", len(query)) + query)
        await writer.drain()
        size = struct.unpack("!H", await reader.readexactly(2))[0]
        answer = await reader.readexactly(size)
        if len(answer) < 12 or answer[:2] != query[:2] or not answer[2] & 128:
            raise ProxyError()
        return answer
    finally:
        writer.close()


class Broker:
    def __init__(self, config):
        self.config = validate(config)
        self.failed = asyncio.Event()
        self.ready = asyncio.Event()
        self.tasks = set()

    async def accept(self, reader, writer):
        task = asyncio.current_task()
        self.tasks.add(task)
        upstream = None
        copies = []
        established = False
        try:
            header = await asyncio.wait_for(reader.readline(), 15)
            if len(header) > MAX_RECORD or not header.endswith(b"\n"):
                raise ProxyError()
            request = json.loads(header)
            if request.get("op") == "ready":
                self.ready.set()
                writer.write(b"OK\n")
                await writer.drain()
                # This connection is the lifetime link to the namespace.
                await reader.read()
                return
            if not self.ready.is_set() or self.failed.is_set():
                raise ProxyError()
            destination = (str(ipaddress.ip_address(request["host"])), request["port"])
            if type(destination[1]) is not int or not 1 <= destination[1] <= 65535:
                raise ProxyError()
            op = request["op"]
            if op not in ("tcp", "udp"):
                raise ProxyError()
            dns = destination == (DNS_ADDRESS, 53)
            if op == "udp" and not dns and self.config["protocol"] != "socks5":
                return  # No UDP fallback for HTTP CONNECT.
            if op == "tcp" and not dns:
                incoming, upstream = await asyncio.wait_for(open_tunnel(self.config, destination), 15)
                writer.write(b"OK\n")
                await writer.drain()
                established = True
                copies = [asyncio.create_task(copy_stream(reader, upstream)),
                          asyncio.create_task(copy_stream(incoming, writer))]
                await asyncio.gather(*copies)
            else:
                writer.write(b"OK\n")
                await writer.drain()
                if not dns:
                    await relay_udp(DatagramStream(reader, writer), self.config, destination)
                elif op == "udp":
                    packets = DatagramStream(reader, writer)
                    while query := await packets.read(65535):
                        packets.write(await asyncio.wait_for(dns_query(self.config, query), 15))
                        await packets.drain()
                else:
                    while True:
                        try:
                            size = struct.unpack("!H", await reader.readexactly(2))[0]
                        except asyncio.IncompleteReadError:
                            break
                        answer = await asyncio.wait_for(dns_query(self.config, await reader.readexactly(size)), 15)
                        writer.write(struct.pack("!H", len(answer)) + answer)
                        await writer.drain()
        except asyncio.CancelledError:
            raise
        except Exception:
            if not established:
                self.failed.set()
        finally:
            for copy in copies:
                copy.cancel()
            await asyncio.gather(*copies, return_exceptions=True)
            if upstream:
                upstream.close()
            writer.close()
            self.tasks.discard(task)

    async def close(self):
        for task in list(self.tasks):
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)


def prepare_prefix(config, base):
    """A different inode and lock prevent reusing any host/other-account wineserver."""
    base = Path(base).resolve(strict=True)
    root = Path(config["state"])
    if not root.is_absolute() or not config.get("account") or not (base / "system.reg").is_file():
        raise ProxyError()
    if root.resolve().is_relative_to(base):
        raise ProxyError()  # Never recursively copy the prefix into itself.
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    if root.is_symlink() or root.stat().st_uid != os.getuid() or root.stat().st_mode & 0o077:
        raise ProxyError()
    key = hashlib.sha256((config["account"] + "\0" + str(base)).encode()).hexdigest()
    folder = root / key
    folder.mkdir(mode=0o700, exist_ok=True)
    if folder.is_symlink():
        raise ProxyError()
    lock = os.open(folder / "lock", os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        prefix = folder / "prefix"
        if prefix.is_symlink():
            raise ProxyError()
        if not prefix.exists():
            # Copy-on-write where supported; never hard-link mutable registry files.
            with tempfile.TemporaryDirectory(prefix="copy-", dir=folder) as staging:
                subprocess.run(["cp", "-a", "--reflink=auto", "--", str(base) + "/.", staging],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                for parent, directories, files in os.walk(staging):
                    for name in directories + files:
                        link = Path(parent) / name
                        if link.is_symlink():
                            target = Path(os.readlink(link))
                            if target.is_absolute() and target.is_relative_to(base):
                                link.unlink()
                                link.symlink_to(prefix / target.relative_to(base))
                os.rename(staging, prefix)
        return prefix, lock
    except BaseException:
        os.close(lock)
        raise


def output_pipe(path):
    """UI closure must not tear down a running client's proxy."""
    connected = True
    try:
        with open(path, "rb", buffering=0) as source:
            while data := source.read(65536):
                if connected:
                    try:
                        offset = 0
                        while offset < len(data):
                            offset += os.write(1, data[offset:])
                    except BrokenPipeError:
                        connected = False
    except OSError:
        pass


def sandbox_command(mitmdump, directory):
    script = Path(__file__).resolve().with_name("kx_proxy_tun.py")
    # No host network interface exists in the child. Its only Internet route is TUN.
    command = ["bwrap", "--unshare-user", "--unshare-net", "--unshare-pid", "--die-with-parent",
            "--cap-add", "CAP_NET_ADMIN", "--bind", "/", "/", "--dev-bind", "/dev", "/dev", "--proc", "/proc",
            "--ro-bind", str(directory / "resolv.conf"), str(Path("/etc/resolv.conf").resolve()),
            "--ro-bind", str(directory / "nsswitch.conf"), str(Path("/etc/nsswitch.conf").resolve())]
    # glibc can consult nscd before nsswitch.conf. Hide the host DNS cache socket.
    for cache in sorted({str(Path(p).resolve()) for p in ("/run/nscd", "/var/run/nscd") if Path(p).is_dir()}):
        command += ["--tmpfs", cache]
    return command + ["--", mitmdump, "-q", "-n", "--set", "server=false", "--set", "termlog_verbosity=error",
                      "-s", str(script), "--set", "kx_linux_dir=" + str(directory)]


async def supervise(config, argv, input_path):
    for name in ("bwrap", "ip", "cp"):
        if not shutil.which(name):
            raise ProxyError()
    preparation = asyncio.create_task(asyncio.to_thread(prepare_prefix, config, os.environ["WINEPREFIX"]))
    while not preparation.done():
        report(0, state=14)
        await asyncio.wait([preparation], timeout=10)
    prefix, lock = await preparation
    child = None
    broker = Broker(config)
    code = 0
    try:
        with tempfile.TemporaryDirectory(prefix="kx-proxy-") as temporary:
            directory = Path(temporary)
            (directory / "resolv.conf").write_text(f"nameserver {DNS_ADDRESS}\noptions timeout:5 attempts:2\n")
            # Avoid systemd-resolved/nscd Unix sockets outside the namespace.
            (directory / "nsswitch.conf").write_text("passwd: files\ngroup: files\nhosts: files dns\n")
            (directory / "launch.json").write_text(json.dumps({"argv": argv + ["--input", input_path],
                "prefix": str(prefix)}))
            os.mkfifo(directory / "output", 0o600)
            output = threading.Thread(target=output_pipe, args=(directory / "output",), daemon=True)
            output.start()
            server = await asyncio.start_unix_server(broker.accept, path=str(directory / "relay"), limit=MAX_RECORD)
            async with server:
                child = await asyncio.create_subprocess_exec(*sandbox_command(config["mitmdump"], directory),
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=None if os.environ.get("KX_PROXY_DIAGNOSTICS") == "1" else subprocess.DEVNULL)
                exited = asyncio.create_task(child.wait())
                ready = asyncio.create_task(broker.ready.wait())
                failed = asyncio.create_task(broker.failed.wait())
                try:
                    done, _ = await asyncio.wait([exited, ready, failed], timeout=45,
                                                 return_when=asyncio.FIRST_COMPLETED)
                    if ready not in done or failed in done or exited in done:
                        code = 1024
                    else:
                        done, _ = await asyncio.wait([exited, failed], return_when=asyncio.FIRST_COMPLETED)
                        if failed in done:
                            code = 1021
                        elif exited.result() != 0:
                            code = 1022
                finally:
                    if child.returncode is None:
                        child.terminate()
                        try:
                            await asyncio.wait_for(asyncio.shield(exited), 5)
                        except asyncio.TimeoutError:
                            child.kill()
                            await exited
                    for task in (ready, failed, exited):
                        task.cancel()
                    await asyncio.gather(ready, failed, exited, return_exceptions=True)
            await broker.close()
            await asyncio.to_thread(output.join, 2)
    finally:
        if child and child.returncode is None:
            child.kill()
            await child.wait()
        await broker.close()
        os.close(lock)
    if code:
        report(code)
    return code


def main():
    global MAGIC
    os.umask(0o077)
    argv = sys.argv[1:]
    if len(argv) < 5 or argv[0] != "--" or argv[-2] != "--input":
        raise ProxyError()
    input_path = argv[-1]
    fd = os.open(input_path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        if not stat.S_ISFIFO(os.fstat(fd).st_mode) or os.fstat(fd).st_uid != os.getuid():
            raise ProxyError()
        length = struct.unpack("<I", read_exact(fd, 4))[0]
        if not 0 < length <= MAX_RECORD:
            raise ProxyError()
        raw = read_exact(fd, length)
        config = json.loads(raw)
        raw[:] = b"\0" * len(raw)
        # Each launcher build has its own helper protocol identifier.
        magic = config.get("magic", MAGIC)
        if type(magic) is not int or not 0 < magic < 2**32:
            raise ProxyError()
        MAGIC = magic
        validate(config)
        return asyncio.run(supervise(config, argv[1:-2], input_path))
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        sys.exit(0 if main() == 0 else 1)
    except Exception:
        # Never expose endpoints, passwords, command lines or packet contents.
        report(1020)
        sys.exit(1)

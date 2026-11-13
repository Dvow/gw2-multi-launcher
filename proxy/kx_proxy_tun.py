"""Runs only inside the account's bubblewrap network/PID namespace."""
import asyncio
import contextlib
import ctypes
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys


def drop_capabilities():
    # TUN configuration needs NET_ADMIN; Wine and its children do not.
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(38, 1, 0, 0, 0) != 0:  # PR_SET_NO_NEW_PRIVS
        os._exit(126)
    libc.prctl(47, 4, 0, 0, 0)  # PR_CAP_AMBIENT_CLEAR_ALL
    header = (ctypes.c_uint32 * 2)(0x20080522, 0)
    data = (ctypes.c_uint32 * 6)()
    if libc.capset(header, data) != 0:
        os._exit(126)


class Tun:
    def load(self, loader):
        loader.add_option("kx_linux_dir", str, "", "Private Linux launcher channel.")

    def running(self):
        self.task = asyncio.create_task(self.run())

    async def stream(self, stream, protocol):
        writer = None
        tasks = []
        try:
            destination = stream.get_extra_info("remote_endpoint", stream.get_extra_info("sockname"))[:2]
            reader, writer = await asyncio.open_unix_connection(str(self.directory / "relay"))
            writer.write(json.dumps({"op": protocol, "host": destination[0], "port": destination[1]}).encode() + b"\n")
            await writer.drain()
            if await asyncio.wait_for(reader.readline(), 20) != b"OK\n":
                return

            async def upload():
                while data := await stream.read(65535):
                    writer.write((struct.pack("!I", len(data)) if protocol == "udp" else b"") + data)
                    await writer.drain()
                with contextlib.suppress(OSError):
                    writer.write_eof()

            async def download():
                while True:
                    if protocol == "udp":
                        size = struct.unpack("!I", await reader.readexactly(4))[0]
                        if not 0 < size <= 65535:
                            return
                        data = await reader.readexactly(size)
                    else:
                        data = await reader.read(65535)
                        if not data:
                            with contextlib.suppress(OSError):
                                stream.write_eof()
                            return
                    stream.write(data)
                    await stream.drain()

            tasks = [asyncio.create_task(upload()), asyncio.create_task(download())]
            await asyncio.gather(*tasks)
        except (OSError, asyncio.IncompleteReadError, asyncio.TimeoutError):
            pass
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            if writer:
                writer.close()
            stream.close()

    async def tcp(self, stream):
        await self.stream(stream, "tcp")

    async def udp(self, stream):
        await self.stream(stream, "udp")

    async def run(self):
        from mitmproxy import ctx
        import mitmproxy_rs
        tun = control = game = None
        monitors = []
        try:
            self.directory = Path(ctx.options.kx_linux_dir)
            # Refuse accidental execution in the host's network namespace.
            if {name for _, name in socket.if_nameindex()} != {"lo"}:
                raise RuntimeError("Network isolation is unavailable")
            tun = await mitmproxy_rs.tun.create_tun_interface(self.tcp, self.udp, tun_name="kxtun0")
            for args in (["link", "set", "lo", "up"],
                         ["route", "replace", "default", "dev", "kxtun0"],
                         ["-6", "addr", "add", "fd00:6b78::1/128", "dev", "kxtun0", "nodad"],
                         ["-6", "route", "replace", "default", "dev", "kxtun0"]):
                subprocess.run(["ip", *args], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            reader, control = await asyncio.open_unix_connection(str(self.directory / "relay"))
            control.write(b'{"op":"ready"}\n')
            await control.drain()
            if await reader.readline() != b"OK\n":
                raise RuntimeError("Proxy broker is unavailable")
            config = json.loads((self.directory / "launch.json").read_text())
            environment = dict(os.environ, WINEPREFIX=config["prefix"])
            environment["STEAM_COMPAT_DATA_PATH"] = config["prefix"]
            # Frozen mitmdump adjusts its own library path. Do not inject its
            # bundled libraries into Wine, Proton or their child applications.
            if getattr(sys, "frozen", False):
                original = environment.pop("LD_LIBRARY_PATH_ORIG", "")
                if original:
                    environment["LD_LIBRARY_PATH"] = original
                else:
                    environment.pop("LD_LIBRARY_PATH", None)
            environment.pop("UMU_CONTAINER_NSENTER", None)
            libraries = environment.get("STEAM_COMPAT_LIBRARY_PATHS", "")
            environment["STEAM_COMPAT_LIBRARY_PATHS"] = libraries + ":" + config["prefix"]
            with open(self.directory / "output", "wb", buffering=0) as output:
                game = await asyncio.create_subprocess_exec(*config["argv"], env=environment,
                    stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.DEVNULL,
                    preexec_fn=drop_capabilities)
                ended = asyncio.create_task(game.wait())
                monitors = [ended, asyncio.ensure_future(tun.wait_closed()), asyncio.create_task(reader.read())]
                done, _ = await asyncio.wait(monitors, return_when=asyncio.FIRST_COMPLETED)
                if ended not in done or ended.result() != 0:
                    raise RuntimeError("Proxy session stopped")
        except Exception:
            if os.environ.get("KX_PROXY_DIAGNOSTICS") == "1":
                import traceback
                traceback.print_exc()
            # A nonzero exit tells the host to report failure. The PID namespace
            # is destroyed with this process, killing Wine and all descendants.
            ctx.master.shutdown()
            os._exit(1)
        finally:
            for task in monitors:
                task.cancel()
            if game and game.returncode is None:
                game.kill()
                await game.wait()
            if tun:
                tun.close()
            if control:
                control.close()
            await asyncio.gather(*monitors, return_exceptions=True)
            ctx.master.shutdown()


addons = [Tun()]

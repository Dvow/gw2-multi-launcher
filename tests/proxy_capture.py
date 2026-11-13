"""Opt-in integration test: Windows may ask to elevate mitmproxy's capture helper."""
import argparse
import asyncio
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "proxy"))
import kx_proxy


async def main(args):
    visits = {"A": 0, "B": 0}
    errors = []
    rejected = False
    datagrams = []
    class UdpProxy(asyncio.DatagramProtocol):
        def connection_made(self, transport):
            self.transport = transport
        def datagram_received(self, data, address):
            try:
                assert data == b"\0\0\0\x01\xcb\x00\x71\x07\x17\xe0udp", data
                datagrams.append(data)
                self.transport.sendto(data[:-3] + b"A:udp", address)
            except Exception as error:
                errors.append(error)
    udp, _ = await asyncio.get_running_loop().create_datagram_endpoint(UdpProxy, local_addr=("127.0.0.1", 0))

    async def upstream(tag, protocol, reader, writer):
        try:
            if protocol == "socks5":
                assert await reader.readexactly(3) == b"\x05\x01\x00"
                if rejected:
                    writer.write(b"\x05\xff")
                    await writer.drain()
                    return
                writer.write(b"\x05\x00")
                await writer.drain()
                command = await reader.readexactly(3)
                if command == b"\x05\x03\x00":
                    assert await kx_proxy.read_address(reader) == ("0.0.0.0", 0)
                    writer.write(b"\x05\x00\x00" + kx_proxy.address_bytes(udp.get_extra_info("sockname")))
                    await writer.drain()
                    await reader.read()
                    return
                assert command == b"\x05\x01\x00"
                assert await kx_proxy.read_address(reader) == ("203.0.113.7", 6112)
                writer.write(b"\x05\x00\x00\x01\0\0\0\0\0\0")
            else:
                header = await reader.readuntil(b"\r\n\r\n")
                assert header.startswith(b"CONNECT 203.0.113.7:6112 HTTP/1.1\r\n")
                writer.write(b"HTTP/1.1 200 OK\r\n\r\n")
            await writer.drain()
            for payload in (b"first", b"second"):
                assert await reader.readexactly(len(payload)) == payload
                visits[tag] += 1
                writer.write(tag.encode() + b":" + payload)
                await writer.drain()
        except Exception as error:
            errors.append(error)
        finally:
            writer.close()

    servers = [await asyncio.start_server(lambda r, w: upstream("A", "socks5", r, w), "127.0.0.1", 0),
               await asyncio.start_server(lambda r, w: upstream("B", "http", r, w), "127.0.0.1", 0)]
    script = args.script or str(pathlib.Path(__file__).resolve().parents[1] / "proxy/kx_proxy.py")
    async def launch(index, protocol, tag, expected=0):
        process = await asyncio.create_subprocess_exec(args.probe, args.mitmdump, script, protocol,
            str(servers[index].sockets[0].getsockname()[1]), tag, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            output, _ = await asyncio.wait_for(process.communicate(), 90)
            print(output.decode(errors="replace"), end="")
            assert process.returncode == expected, (tag, process.returncode)
            if expected:
                assert b"Proxy failure: 1021" in output, output
        finally:
            if process.returncode is None:
                process.kill()
                await process.wait()
    try:
        await asyncio.gather(launch(0, "socks5", "A"), launch(1, "http", "B"))
        assert not errors, errors
        assert visits == {"A": 2, "B": 2}, visits
        assert len(datagrams) == 1, datagrams
        print("Real capture: separate TCP routes, SOCKS5 UDP, blocked HTTP UDP, surviving account cleanup.")
        rejected = True
        visits = {"A": 0, "B": 0}
        await asyncio.gather(launch(0, "socks5", "A", 21), launch(1, "http", "B"))
        assert visits == {"A": 0, "B": 2}, visits
        assert not errors, errors
        print("Rejected proxy stops only its client; the other account remains connected.")
    finally:
        for server in servers:
            server.close()
            await server.wait_closed()
        udp.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--mitmdump", required=True)
    parser.add_argument("--script")
    asyncio.run(main(parser.parse_args()))

"""Windows control-pipe lifetime tests without a capture driver or elevation."""
import asyncio
import ctypes
from ctypes import wintypes
import pathlib
import sys
import threading
import unittest
import uuid

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "proxy"))
from kx_proxy_pipe import WindowsPipes


@unittest.skipUnless(sys.platform == "win32", "Windows named pipes")
class ControlTests(unittest.IsolatedAsyncioTestCase):
    async def test_shutdown_waits_for_native_startup_mutex(self):
        server = WindowsPipes(r"\\.\pipe\KXProxy-test-" + uuid.uuid4().hex,
                              asyncio.get_running_loop(), lambda *_: None)
        locked, release = threading.Event(), threading.Event()
        def native_startup():
            self.assertEqual(server.kernel.WaitForSingleObject(server.mutex, 1000), 0)
            locked.set()
            release.wait(3)
            server.kernel.ReleaseMutex(server.mutex)
        thread = threading.Thread(target=native_startup)
        thread.start()
        try:
            self.assertTrue(await asyncio.to_thread(locked.wait, 2))
            self.assertFalse(server.begin_shutdown())
            release.set()
            await asyncio.to_thread(thread.join, 2)
            self.assertTrue(server.begin_shutdown())
        finally:
            release.set()
            thread.join(2)
            server.close()
        self.assertFalse(server.thread.is_alive())
        # The stopped service must have relinquished the name, not just its loop.
        replacement = WindowsPipes(server.name, asyncio.get_running_loop(), lambda *_: None)
        replacement.close()
        self.assertFalse(replacement.thread.is_alive())

    async def test_private_request_and_heartbeat(self):
        accepted = asyncio.Future()
        def accept(connection, request):
            accepted.set_result((connection, request))
        server = WindowsPipes(r"\\.\pipe\KXProxy-test-" + uuid.uuid4().hex, asyncio.get_running_loop(), accept)
        kernel = server.kernel
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                      wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        handle = kernel.CreateFileW(server.name, 0xC0000000, 0, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        connection = None
        try:
            request, count = b'{"test":"private"}', wintypes.DWORD()
            self.assertTrue(kernel.WriteFile(handle, request, len(request), ctypes.byref(count), None))
            connection, received = await asyncio.wait_for(accepted, 2)
            self.assertEqual(received, request)
            self.assertEqual(connection.owner, __import__('os').getpid())
            connection.report("ALIVE")
            buffer = ctypes.create_string_buffer(64)
            self.assertTrue(kernel.ReadFile(handle, buffer, len(buffer), ctypes.byref(count), None))
            self.assertEqual(buffer.raw[:count.value], b"KXPROXY ALIVE\n")
            kernel.CloseHandle(handle)
            handle = None
            self.assertTrue(await asyncio.to_thread(connection.disconnected.wait, 2))
        finally:
            if handle:
                kernel.CloseHandle(handle)
            if connection:
                connection.close()
            server.close()


if __name__ == "__main__":
    unittest.main()

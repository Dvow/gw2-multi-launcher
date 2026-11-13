"""Windows-only, same-user named-pipe control channel for the launcher's capture service."""
import ctypes as c
from ctypes import wintypes as w
import threading
import time


class WindowsPipes:
    def __init__(self, name, loop, accept):
        self.loop, self.accept, self.name = loop, accept, name
        self.kernel = c.WinDLL("kernel32", use_last_error=True)
        self.security = c.WinDLL("advapi32", use_last_error=True)
        self.kernel.CreateNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, c.c_void_p]
        self.kernel.CreateNamedPipeW.restype = w.HANDLE
        self.kernel.ConnectNamedPipe.argtypes = [w.HANDLE, c.c_void_p]
        self.kernel.CreateMutexW.argtypes = [c.c_void_p, w.BOOL, w.LPCWSTR]
        self.kernel.CreateMutexW.restype = w.HANDLE
        self.kernel.ReleaseMutex.argtypes = [w.HANDLE]
        self.kernel.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.kernel.OpenThread.restype = w.HANDLE
        self.kernel.CancelSynchronousIo.argtypes = [w.HANDLE]
        self.kernel.DisconnectNamedPipe.argtypes = [w.HANDLE]
        self.kernel.CloseHandle.argtypes = [w.HANDLE]
        self.kernel.ReadFile.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
        self.kernel.WriteFile.argtypes = self.kernel.ReadFile.argtypes
        self.kernel.GetNamedPipeClientProcessId.argtypes = [w.HANDLE, c.POINTER(w.ULONG)]
        self.kernel.PeekNamedPipe.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.c_void_p, c.c_void_p, c.c_void_p]
        self.kernel.SetNamedPipeHandleState.argtypes = [w.HANDLE, c.POINTER(w.DWORD), c.c_void_p, c.c_void_p]
        self.kernel.GetCurrentProcess.restype = w.HANDLE
        self.kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.kernel.OpenProcess.restype = w.HANDLE
        self.kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
        self.kernel.LocalFree.argtypes = [c.c_void_p]
        self.security.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, c.POINTER(w.HANDLE)]
        self.security.GetTokenInformation.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)]
        self.security.ConvertSidToStringSidW.argtypes = [c.c_void_p, c.POINTER(w.LPWSTR)]
        self.security.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [w.LPCWSTR, w.DWORD, c.POINTER(c.c_void_p), c.c_void_p]
        self.descriptor = self._descriptor()
        self.mutex = self.kernel.CreateMutexW(None, False, "Local\\" + name.rsplit("\\", 1)[1])
        if not self.mutex:
            raise OSError("Cannot synchronize proxy startup")
        self.shutdown_locked = False
        self.stopped = False
        self.listener = None
        # Create synchronously, so a competing service cannot also claim the name.
        self.first = self._create(True)
        self.thread = threading.Thread(target=self._listen, daemon=True)
        self.thread.start()

    def _descriptor(self):
        token, length = w.HANDLE(), w.DWORD()
        if not self.security.OpenProcessToken(self.kernel.GetCurrentProcess(), 8, c.byref(token)):
            raise OSError("Cannot protect proxy control pipe")
        try:
            self.security.GetTokenInformation(token, 1, None, 0, c.byref(length))
            data = c.create_string_buffer(length.value)
            if not self.security.GetTokenInformation(token, 1, data, length, c.byref(length)):
                raise OSError("Cannot read proxy user")
            sid = c.cast(data, c.POINTER(c.c_void_p))[0]
            text = w.LPWSTR()
            if not self.security.ConvertSidToStringSidW(sid, c.byref(text)):
                raise OSError("Cannot identify proxy user")
            try:
                sddl = f"D:P(A;;GA;;;{text.value})"
            finally:
                self.kernel.LocalFree(c.cast(text, c.c_void_p))
            descriptor = c.c_void_p()
            if not self.security.ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, 1, c.byref(descriptor), None):
                raise OSError("Cannot protect proxy control pipe")
            return descriptor
        finally:
            self.kernel.CloseHandle(token)

    def _create(self, first=False):
        class Attributes(c.Structure):
            _fields_ = [("length", w.DWORD), ("descriptor", c.c_void_p), ("inherit", w.BOOL)]
        attributes = Attributes(c.sizeof(Attributes), self.descriptor, False)
        # Duplex, message mode, reject remote clients, no handle inheritance.
        handle = self.kernel.CreateNamedPipeW(self.name, 3 | (0x80000 if first else 0),
                                              4 | 2 | 8, 255, 4096, 16384, 0, c.byref(attributes))
        if handle == c.c_void_p(-1).value:
            raise OSError("Cannot open proxy control pipe")
        return handle

    def _listen(self):
        handle = self.first
        try:
            while not self.stopped:
                self.listener = handle
                connected = self.kernel.ConnectNamedPipe(handle, None) or c.get_last_error() == 535
                if not connected or self.stopped:
                    break
                threading.Thread(target=self._read, args=(handle,), daemon=True).start()
                handle = None  # The connection reader now owns this instance.
                try:
                    handle = self._create()
                except OSError:
                    self.loop.call_soon_threadsafe(self.accept, None, None)
                    break
        finally:
            if handle:
                self.kernel.CloseHandle(handle)
            self.listener = None

    def _read(self, handle):
        connection = PipeConnection(self, handle)
        buffer, count, owner = c.create_string_buffer(16384), w.DWORD(), w.ULONG()
        if (self.kernel.GetNamedPipeClientProcessId(handle, c.byref(owner)) and
                self.kernel.ReadFile(handle, buffer, len(buffer), c.byref(count), None)):
            connection.owner = owner.value
            mode = w.DWORD(2 | 1)  # Message reads, nonblocking writes after the first record.
            if not self.kernel.SetNamedPipeHandleState(handle, c.byref(mode), None, None):
                connection.close()
                return
            self.loop.call_soon_threadsafe(self.accept, connection, buffer.raw[:count.value])
            while connection.handle and not connection.disconnected.is_set():
                if not self.kernel.PeekNamedPipe(handle, None, 0, None, None, None):
                    break
                time.sleep(0.1)
        else:
            connection.close()
        connection.disconnected.set()

    def begin_shutdown(self):
        # Native hosts hold this mutex until their suspended game has a verified
        # route. An idle service must not close the pipe during a new handshake.
        result = self.kernel.WaitForSingleObject(self.mutex, 0)
        if result not in (0, 0x80):
            return False
        self.shutdown_locked = True
        return True

    def close(self):
        self.stopped = True
        thread = self.kernel.OpenThread(1, False, self.thread.native_id)
        if thread:
            try:
                # Repeat cancellation to cover the transition into ConnectNamedPipe.
                for _ in range(20):
                    self.kernel.CancelSynchronousIo(thread)
                    self.thread.join(0.05)
                    if not self.thread.is_alive():
                        break
            finally:
                self.kernel.CloseHandle(thread)
        if self.shutdown_locked:
            self.kernel.ReleaseMutex(self.mutex)
        self.kernel.CloseHandle(self.mutex)
        if not self.thread.is_alive():
            self.kernel.LocalFree(self.descriptor)


class PipeConnection:
    def __init__(self, server, handle):
        self.server, self.handle = server, handle
        self.owner = 0
        self.disconnected = threading.Event()

    def report(self, status):
        data = ("KXPROXY " + status + "\n").encode("ascii")
        count = w.DWORD()
        if not self.server.kernel.WriteFile(self.handle, data, len(data), c.byref(count), None) or count.value != len(data):
            self.disconnected.set()

    def game_handle(self, pid):
        handle = self.server.kernel.OpenProcess(0x100000, False, pid)  # SYNCHRONIZE only
        if not handle:
            raise OSError("Cannot monitor proxy client")
        return handle

    def exited(self, handle):
        return self.server.kernel.WaitForSingleObject(handle, 0) != 258

    def close(self):
        if self.handle:
            self.server.kernel.DisconnectNamedPipe(self.handle)
            self.server.kernel.CloseHandle(self.handle)
            self.handle = None

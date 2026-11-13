#pragma once
#include <windows.h>
#include <tlhelp32.h>

namespace gw2_exit {
// TerminateProcess only queues each thread's exit; the threads still run it at their own
// priority, and the last one tears the process down. A killed GW2 kept threads at priority 1,
// so with a busy CPU it took 30-120 s to exit. Call after terminating: no thread can start then.
inline void hasten(DWORD pid) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) continue;
        if (const HANDLE thread = OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, entry.th32ThreadID)) {
            SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
            CloseHandle(thread);
        }
    }
    CloseHandle(snapshot);
}
} // namespace gw2_exit

#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

// Read the native stack of our own game's named thread, never unrelated apps.
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 2;
    const DWORD pid = wcstoul(argv[1], nullptr, 10);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return 3;
    wchar_t path[32768]{};
    DWORD length = 32768;
    if (!QueryFullProcessImageNameW(process, 0, path, &length) ||
        std::wstring(path).find(L"WarriorsOrochi.exe") == std::wstring::npos) {
        CloseHandle(process); return 4;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitializeW(process, nullptr, TRUE)) { CloseHandle(process); return 5; }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    unsigned captured = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) continue;
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, entry.th32ThreadID);
        if (!thread) continue;
        PWSTR description = nullptr;
        const bool gameThread = SUCCEEDED(GetThreadDescription(thread, &description)) && description &&
                                std::wstring(description) == L"GameThread";
        if (description) LocalFree(description);
        if (!gameThread) { CloseHandle(thread); continue; }
        std::cout << "[native-stack] pid=" << pid << " tid=" << entry.th32ThreadID << '\n';
        if (SuspendThread(thread) != DWORD(-1)) {
            CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
                for (unsigned i = 0; i < 48; ++i) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                                     nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                        !frame.AddrPC.Offset) break;
                    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 2048]{};
                    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
                    symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 2047;
                    DWORD64 displacement = 0;
                    std::cout << '#' << i << " 0x" << std::hex << frame.AddrPC.Offset;
                    if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
                        std::cout << ' ' << symbol->Name << "+0x" << displacement;
                    std::cout << std::dec << '\n';
                }
                ++captured;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot); SymCleanup(process); CloseHandle(process);
    std::cout << "[native-stack] captured=" << captured << '\n';
    return captured ? 0 : 6;
}

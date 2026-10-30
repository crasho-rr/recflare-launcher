// Built-in loader for the Rec Room 2026 patch (client 20260323.14).
//
// RecRoom.exe gets an extra import of 2026loader.dll!RecFlareLoader (see
// addimport.cpp), so Windows loads this DLL before the game starts. On attach
// it does in-process what Injector.exe does from outside:
//   1. supplies the local Referee arena before its first dereference, and
//   2. loads 2026patch.dll once GameAssembly.dll is present.
// Only RecRoom.exe is edited; the three fingerprinted DLLs stay untouched.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <cstdio>
#include <share.h>

namespace {

// Same measured values as RR::Methods::Referee in patch-2026.
constexpr uintptr_t ArenaSlot = 0x6C10858;
constexpr uintptr_t BootstrapInstruction = 0x69839D;
constexpr SIZE_T ArenaSize = 0x04000000;

wchar_t g_dir[MAX_PATH];
uintptr_t g_referee;
volatile LONG g_arenaDone;
DWORD g_mainThread;
FILE* g_log;

void Log(const char* fmt, ...) {
    if (!g_log) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    fprintf(g_log, "[%llu] ", GetTickCount64());
    vfprintf(g_log, fmt, args);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(args);
}

bool SafeRead(uintptr_t address, void* out, size_t size) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Mirrors Injector.exe: three channel descriptors at stride 0x1500030, each
// with two length-prefixed queues. Headers are written before the arena
// pointer is published. Returns the arena now in the slot (ours or existing).
uintptr_t EnsureArena(uintptr_t* slot) {
    uintptr_t existing = 0;
    if (!SafeRead(reinterpret_cast<uintptr_t>(slot), &existing, sizeof(existing)) || existing) {
        return existing;
    }
    auto arena = reinterpret_cast<uintptr_t>(
        VirtualAlloc(nullptr, ArenaSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!arena) {
        return 0;
    }
    for (unsigned channel = 0; channel < 3; ++channel) {
        const uintptr_t segment = arena + channel * 0x1500030ull;
        const uintptr_t offsets[2] = {0xA0, 0x100040};
        const uint32_t lengths[2] = {0x80000, 0x1000000};
        for (unsigned queue = 0; queue < 2; ++queue) {
            const uint32_t capacity = (lengths[queue] - 0x1C) / 2;
            const uint32_t header[8] = {lengths[queue], 0, 0, capacity, 0, 0, capacity, 0};
            memcpy(reinterpret_cast<void*>(segment + offsets[queue]), header, sizeof(header));
        }
    }
    auto prior = reinterpret_cast<uintptr_t>(InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(arena), nullptr));
    if (prior) {
        // Someone (e.g. a manually started Injector.exe) got there first.
        VirtualFree(reinterpret_cast<void*>(arena), 0, MEM_RELEASE);
        return prior;
    }
    return arena;
}

// Returns true once the slot pointer is initialized and holds an arena.
bool TryBootstrap() {
    unsigned char instruction[3]{};
    uintptr_t slot = 0;
    if (!SafeRead(g_referee + BootstrapInstruction, instruction, 3) || memcmp(instruction, "\x41\x8B\x00", 3) ||
        !SafeRead(g_referee + ArenaSlot, &slot, sizeof(slot)) || !slot) {
        return false;
    }
    const uintptr_t arena = EnsureArena(reinterpret_cast<uintptr_t*>(slot));
    if (arena && !InterlockedExchange(&g_arenaDone, 1)) {
        Log("arena ready at %p", reinterpret_cast<void*>(arena));
    }
    return arena != 0;
}

// Referee initializes the slot and dereferences the arena inside its own
// startup, under the loader lock, so no thread of ours can run in between.
// It loads further DLLs in that window, and DLL notifications are delivered
// synchronously on the loading thread: supply the arena from there.
struct DllNotificationData {
    ULONG flags;
    const UNICODE_STRING* fullName;
    const UNICODE_STRING* baseName;
    void* base;
    ULONG size;
};
using DllNotificationFunction = void(CALLBACK*)(ULONG reason, const DllNotificationData* data, void* context);
using LdrRegisterDllNotificationFn = LONG(NTAPI*)(ULONG, DllNotificationFunction, void*, void**);
using LdrUnregisterDllNotificationFn = LONG(NTAPI*)(void*);
void* g_cookie;

void CALLBACK OnDllNotification(ULONG reason, const DllNotificationData* data, void*) {
    if (g_arenaDone || reason != 1) {
        return;
    }
    if (!g_referee) {
        g_referee = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Referee.dll"));
    }
    const bool ready = g_referee && TryBootstrap();
    Log("dll loaded: %.*ls (arena %s)", data->baseName->Length / 2, data->baseName->Buffer, ready ? "ready" : "not yet");
}

// 2026patch.dll SHA-256s these modules from disk before installing any hook.
// From a cold cache on a hard drive that takes seconds and the client reaches
// its name-server request first. Read them into the file cache early.
DWORD WINAPI Prewarm(void*) {
    static char buffer[1 << 20];
    static const wchar_t* const names[] = {L"GameAssembly.dll", L"Referee.dll", L"UnityPlayer.dll"};
    for (const wchar_t* name : names) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%s", g_dir, name);
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            continue;
        }
        DWORD read = 0;
        while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read) {
        }
        CloseHandle(file);
    }
    Log("game modules prewarmed");
    return 0;
}

bool PatchReported(const wchar_t* logPath) {
    // Any of these lines ends the patch's startup: success, or a reported failure.
    static const char* const finals[] = {"[Build] hooks enabled", "[Build] initialization failed",
                                         "[Build] fingerprint mismatch", "[Config]"};
    FILE* f = _wfsopen(logPath, L"r", _SH_DENYNO);
    if (!f) {
        return false;
    }
    char line[512];
    bool done = false;
    while (!done && fgets(line, sizeof(line), f)) {
        for (const char* final : finals) {
            done = done || !strncmp(line, final, strlen(final));
        }
    }
    fclose(f);
    return done;
}

// Hold the client back until the patch has hooked, so its first requests are
// already redirected. The main thread mostly sleeps but gets short slices, so
// a lock it holds can never stall the patch for good; capped at 20 seconds.
void GateUntilPatched(const wchar_t* logPath) {
    if (PatchReported(logPath)) {
        return;
    }
    HANDLE main = OpenThread(THREAD_SUSPEND_RESUME, FALSE, g_mainThread);
    const ULONGLONG start = GetTickCount64();
    while (!PatchReported(logPath) && GetTickCount64() - start < 20000) {
        if (main) {
            SuspendThread(main);
        }
        Sleep(40);
        if (main) {
            ResumeThread(main);
        }
        Sleep(4);
    }
    if (main) {
        CloseHandle(main);
    }
    Log("held the client %llu ms for patch startup", GetTickCount64() - start);
}

DWORD WINAPI Worker(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    wchar_t path[MAX_PATH];
    Log("worker running");
    if (HANDLE thread = CreateThread(nullptr, 0, Prewarm, nullptr, 0, nullptr)) {
        CloseHandle(thread);
    }

    const ULONGLONG deadline = GetTickCount64() + 60000;
    while (!g_arenaDone && GetTickCount64() < deadline) {
        if (!g_referee) {
            g_referee = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Referee.dll"));
        }
        if (g_referee && TryBootstrap()) {
            break;
        }
        SwitchToThread();
    }
    if (!g_arenaDone) {
        Log("arena slot never initialized; not loading the patch");
        return 1;
    }
    if (g_cookie) {
        auto unregister = reinterpret_cast<LdrUnregisterDllNotificationFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrUnregisterDllNotification"));
        unregister(g_cookie);
        g_cookie = nullptr;
    }

    while (!GetModuleHandleW(L"GameAssembly.dll")) {
        Sleep(10);
    }
    if (GetModuleHandleW(L"2026patch.dll")) {
        Log("2026patch.dll already loaded (injector?), skipping");
        return 0;
    }
    // The gate reads the patch log, so the previous launch's log must not count.
    swprintf_s(path, L"%s\\2026patch.log", g_dir);
    DeleteFileW(path);
    swprintf_s(path, L"%s\\2026patch.dll", g_dir);
    HMODULE patch = LoadLibraryW(path);
    if (!patch) {
        Log("LoadLibrary(2026patch.dll) failed: %lu", GetLastError());
        return 1;
    }
    Log("2026patch.dll loaded; see 2026patch.log for hook status");
    swprintf_s(path, L"%s\\2026patch.log", g_dir);
    GateUntilPatched(path);
    return 0;
}

} // namespace

// Import target for the edited RecRoom.exe; never called.
extern "C" __declspec(dllexport) void RecFlareLoader() {}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    DisableThreadLibraryCalls(instance);
    // Only act inside the game itself, not in any helper that shares the folder.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* exeName = wcsrchr(exe, L'\\');
    if (_wcsicmp(exeName ? exeName + 1 : exe, L"RecRoom.exe")) {
        return TRUE;
    }
    GetModuleFileNameW(instance, g_dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) {
        *slash = 0;
    }
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\2026loader.log", g_dir);
    g_log = _wfsopen(path, L"w", _SH_DENYNO);
    Log("built-in loader attached");
    g_referee = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Referee.dll"));
    // Static imports attach on the initial thread, which goes on to run the game.
    g_mainThread = GetCurrentThreadId();
    auto registerNotification = reinterpret_cast<LdrRegisterDllNotificationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    if (!registerNotification || registerNotification(0, OnDllNotification, nullptr, &g_cookie) < 0) {
        Log("LdrRegisterDllNotification unavailable");
    }
    // Loader lock is held: start the worker and never wait for it here. It
    // runs once process startup finishes and loads the patch.
    if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) {
        CloseHandle(thread);
    }
    return TRUE;
}

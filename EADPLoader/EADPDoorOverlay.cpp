// EADPDoorOverlay.exe – launcher / injector for EADPDoorHook.dll.
//
// 1. Optionally launches the game exe passed as a command-line argument.
// 2. Finds the game window using four fallback strategies.
// 3. Injects EADPDoorHook.dll into the game process.
// 4. Waits for the game to exit, then cleans up.
//
// All rendering is handled inside EADPDoorHook.dll via D3D9 EndScene.
//
// Usage:
//   EADPDoorOverlay.exe               – attach to already-running game
//   EADPDoorOverlay.exe "game.exe"    – launch game then inject

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <string>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")

#define GAME_CLASS "Eva"
#define GAME_TITLE "OpenParrot - Elevator Action: Death Parade"

// ---------------------------------------------------------------------------
// Game-window search (4 strategies)
// ---------------------------------------------------------------------------

struct FindByPid { DWORD pid; HWND hwnd; };
static BOOL CALLBACK FindByPidProc(HWND hw, LPARAM lp)
{
    auto* d = (FindByPid*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    if (pid == d->pid && IsWindowVisible(hw)) { d->hwnd = hw; return FALSE; }
    return TRUE;
}
static HWND WindowFromPid(DWORD pid)
{
    FindByPid d = { pid, NULL };
    EnumWindows(FindByPidProc, (LPARAM)&d);
    return d.hwnd;
}

static DWORD PidFromExeName(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe))
        do { if (_wcsicmp(pe.szExeFile, name) == 0) { found = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

static HWND FindGameWindow(DWORD launchedPid)
{
    // 1. Exact: class + OpenParrot-patched title
    HWND hw = FindWindowA(GAME_CLASS, GAME_TITLE);
    if (hw) return hw;
    // 2. Class-only (game without OpenParrot renaming)
    hw = FindWindowA(GAME_CLASS, NULL);
    if (hw) return hw;
    // 3. Any visible window of the process we launched
    if (launchedPid) { hw = WindowFromPid(launchedPid); if (hw) return hw; }
    // 4. Scan for game.exe
    DWORD gPid = PidFromExeName(L"game.exe");
    if (gPid) return WindowFromPid(gPid);
    return NULL;
}

// ---------------------------------------------------------------------------
// DLL injection via CreateRemoteThread + LoadLibraryW
// ---------------------------------------------------------------------------

static bool InjectDLL(DWORD pid)
{
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    PathRemoveFileSpecW(dir);
    std::wstring dllPath = std::wstring(dir) + L"\\EADPDoorHook.dll";

    HANDLE hProc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!hProc) return false;

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(hProc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(hProc); return false; }

    WriteProcessMemory(hProc, remote, dllPath.c_str(), bytes, NULL);

    FARPROC loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE hThread  = CreateRemoteThread(hProc, NULL, 0,
        (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
    if (!hThread) { VirtualFreeEx(hProc, remote, 0, MEM_RELEASE); CloseHandle(hProc); return false; }

    WaitForSingleObject(hThread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return exitCode != 0;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int)
{
    // The game MUST be started through OpenParrotLoader (or TeknoParrot) first
    // so that OpenParrot's patches are applied before the game runs.
    // Launching game.exe directly here would crash because those patches are missing.
    //
    // If a path was passed on the command line, warn the user and ignore it.
    if (lpCmdLine && lpCmdLine[0])
    {
        MessageBoxW(NULL,
            L"EADPDoorOverlay does not launch the game directly.\n\n"
            L"Please start the game through OpenParrotLoader first:\n"
            L"  OpenParrotLoader.exe OpenParrot game.exe\n\n"
            L"Then run EADPDoorOverlay.exe (no arguments) to attach.",
            L"EADP Door Overlay", MB_ICONINFORMATION);
        return 0;
    }

    // Wait up to 120 s for the game window
    HWND hwndGame = NULL;
    for (int i = 0; i < 1200 && !hwndGame; ++i)
    {
        hwndGame = FindGameWindow(0);
        if (!hwndGame) Sleep(100);
    }
    if (!hwndGame)
    {
        wchar_t msg[512];
        swprintf_s(msg,
            L"Game window not found after 120 s.\n\n"
            L"Strategies tried:\n"
            L"  1. FindWindow class=\"Eva\" title=\"%hs\"\n"
            L"  2. FindWindow class=\"Eva\" (any title)\n"
            L"  3. Window of launched PID %lu\n"
            L"  4. Process named game.exe\n\n"
            L"If the game uses a different window class or exe name,\n"
            L"start the game first, then run EADPDoorOverlay.exe with no arguments.",
            GAME_TITLE, pi.dwProcessId);
        MessageBoxW(NULL, msg, L"EADP Door Overlay", MB_ICONERROR);
        return 1;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwndGame, &pid);
    if (!pid) pid = PidFromExeName(L"game.exe");

    if (!InjectDLL(pid))
    {
        MessageBoxW(NULL,
            L"Failed to inject EADPDoorHook.dll.\n"
            L"Make sure it is next to EADPDoorOverlay.exe.",
            L"EADP Door Overlay", MB_ICONERROR);
        return 1;
    }

    // Wait for the game process to exit
    HANDLE hGameProc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (hGameProc) { WaitForSingleObject(hGameProc, INFINITE); CloseHandle(hGameProc); }

    return 0;
}

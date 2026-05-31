// EADPDoorOverlay.exe
// Standalone Windows program that renders door images over the EADP game window.
// Designed to run under Wine on Linux.
//
// Usage:
//   EADPDoorOverlay.exe [path-to-game-exe]
//
//   If a game exe path is supplied the program launches the game first,
//   then waits for its window to appear and injects EADPDoorHook.dll.
//   If no argument is given it just attaches to an already-running game.
//
// Image files expected next to this exe:
//   DoorLeft.png    - left door panel
//   DoorRight.png   - right door panel
//   DoorsBezel.png  - static bezel drawn behind the doors (optional)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <string>
#include "EADPShared.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "kernel32.lib")

using Gdiplus::Bitmap;
using Gdiplus::Graphics;
using Gdiplus::Color;
using Gdiplus::Image;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static HINSTANCE       g_hInstance    = NULL;
static HWND            g_hwndOverlay  = NULL;

static HANDLE          g_hSharedMem   = NULL;
static EADPSharedState* g_pState      = NULL;

static Bitmap*         g_pDoorLeft    = NULL;
static Bitmap*         g_pDoorRight   = NULL;
static Bitmap*         g_pDoorsBezel  = NULL;

static int g_dlW = 0, g_dlH = 0;   // DoorLeft dimensions
static int g_drW = 0, g_drH = 0;   // DoorRight dimensions
static int g_dbW = 0, g_dbH = 0;   // DoorsBezel dimensions

// Door animation state (managed by overlay)
static float g_doorVisLeft  = 0.0f; // current visual position (D3DX center-x)
static float g_doorVisRight = 0.0f; // initialised on first frame
static bool  g_doorInit     = false;

// Vibration state (local to overlay, not stored in shared mem to avoid races)
static bool  g_vibActive        = false;
static int   g_vibCount         = 0;
static int   g_vibSleepCount    = 0;
static bool  g_vibPlayPhase     = false;
static float g_doorFloatLeft    = 0.0f; // current target (may be modulated by vibration)
static float g_doorFloatRight   = 0.0f;
static float g_currentDoorLeft  = 0.0f; // saved "real" target for vibration restore
static float g_currentDoorRight = 0.0f;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

// Load PNG next to the exe
static Bitmap* LoadPNG(const wchar_t* name)
{
    std::wstring path = ExeDir() + L"\\" + name;
    Bitmap* bmp = Bitmap::FromFile(path.c_str());
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok)
    {
        delete bmp;
        return NULL;
    }
    return bmp;
}

// ---------------------------------------------------------------------------
// Inject EADPDoorHook.dll into the target process via CreateRemoteThread
// ---------------------------------------------------------------------------

static bool InjectDLL(DWORD pid)
{
    std::wstring dllPath = ExeDir() + L"\\" + EADP_HOOK_DLL_NAME;

    HANDLE hProcess = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!hProcess)
        return false;

    size_t pathBytes = (wcslen(dllPath.c_str()) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(hProcess, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        CloseHandle(hProcess);
        return false;
    }

    WriteProcessMemory(hProcess, remote, dllPath.c_str(), pathBytes, NULL);

    FARPROC loadLibW = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibW, remote, 0, NULL);

    if (!hThread)
    {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 10000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    return exitCode != 0;
}

// ---------------------------------------------------------------------------
// Shared memory
// ---------------------------------------------------------------------------

static bool OpenSharedMemory()
{
    // The hook DLL creates the mapping; we open it
    for (int retry = 0; retry < 200; ++retry)   // wait up to ~4 s
    {
        g_hSharedMem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, EADP_SHARED_NAME);
        if (g_hSharedMem)
            break;
        Sleep(20);
    }
    if (!g_hSharedMem)
        return false;

    g_pState = (EADPSharedState*)MapViewOfFile(
        g_hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EADPSharedState));

    return g_pState != NULL;
}

// ---------------------------------------------------------------------------
// Door position calculation (mirrors D3D9Misc.cpp EADPAttractionDoorsCalculations)
//
// Positions are "centre" coordinates matching D3DX Sprite semantics.
// GDI+ top-left = centre - halfSize.
// ---------------------------------------------------------------------------

static float g_leftMin  = 0.0f, g_leftMax  = 0.0f;
static float g_rightMin = 0.0f, g_rightMax = 0.0f;
static float g_doorSpeed = 8.0f;

static void InitDoorRange(int winW)
{
    float cx = (float)(int)round((float)winW / 2.0f);

    g_leftMin  = cx - (float)g_dlW * 2.0f + (float)g_dlW / 2.0f;
    g_leftMax  = cx - (float)g_dlW / 2.0f;

    g_rightMax = cx + (float)g_drW / 2.0f;
    g_rightMin = cx + (float)g_drW * 2.0f - (float)g_drW / 2.0f;

    if (!g_doorInit)
    {
        g_doorVisLeft  = g_leftMin;
        g_doorVisRight = g_rightMin;
        g_doorInit = true;
    }

    // Speed scales proportionally to render width (same formula as original)
    g_doorSpeed = ((float)g_dlW / 2.0f) / 405.0f * 8.0f;
    if (g_doorSpeed < 1.0f) g_doorSpeed = 1.0f;
}

static void StepDoorAnimation()
{
    // Read latest door targets from shared memory (if not vibrating)
    if (!g_vibActive && g_pState)
    {
        float newLeft, newRight;
        // Read atomically via LONG alias
        *(LONG*)&newLeft  = InterlockedCompareExchange((LONG*)&g_pState->AttractionDoorLeft,  0, 0);
        *(LONG*)&newRight = InterlockedCompareExchange((LONG*)&g_pState->AttractionDoorRight, 0, 0);

        g_doorFloatLeft    = newLeft;
        g_doorFloatRight   = newRight;
        g_currentDoorLeft  = newLeft;
        g_currentDoorRight = newRight;
    }

    // Check for new vibration trigger
    if (g_pState && !g_vibActive)
    {
        if (InterlockedCompareExchange(&g_pState->VibrationEffect, 0, 0) != 0)
        {
            g_vibActive     = true;
            g_vibCount      = 0;
            g_vibSleepCount = 0;
            g_vibPlayPhase  = false;
        }
    }

    // Vibration modulation (mirrors EADPInputs.cpp)
    if (g_vibActive && g_pState)
    {
        ++g_vibCount;
        ++g_vibSleepCount;

        float power = (float)InterlockedCompareExchange(&g_pState->VibrationPower, 0, 0) / 200.0f;

        if (g_vibSleepCount >= 2)
        {
            g_vibSleepCount = 0;
            g_vibPlayPhase  = !g_vibPlayPhase;
        }

        if (g_vibPlayPhase)
        {
            g_doorFloatLeft  += power;
            g_doorFloatRight += power;
        }
        else
        {
            g_doorFloatLeft  -= power;
            g_doorFloatRight -= power;
        }

        g_doorFloatLeft  = std::max(0.0f, std::min(1.0f, g_doorFloatLeft));
        g_doorFloatRight = std::max(0.0f, std::min(1.0f, g_doorFloatRight));

        int vibTime = (int)InterlockedCompareExchange(&g_pState->VibrationTime, 0, 0);
        if (g_vibCount >= (int)(vibTime / 16.0f))
        {
            g_vibActive      = false;
            g_doorFloatLeft  = g_currentDoorLeft;
            g_doorFloatRight = g_currentDoorRight;
            // Clear the flag so the hook can trigger it again
            InterlockedExchange(&g_pState->VibrationEffect, 0L);
        }
    }

    // Slide visual positions toward targets
    float leftTotal  = g_leftMax  - g_leftMin;
    float rightTotal = g_rightMin - g_rightMax;  // positive (rightMin > rightMax)

    float targetLeft  = g_doorFloatLeft  * leftTotal  + g_leftMin;
    float targetRight = g_rightMin - g_doorFloatRight * rightTotal; // rightMin - float*range → moves toward rightMax

    // Clamp visual positions within valid range
    g_doorVisLeft  = std::max(g_leftMin,  std::min(g_leftMax,  g_doorVisLeft));
    g_doorVisRight = std::max(g_rightMax, std::min(g_rightMin, g_doorVisRight));

    // Smooth slide
    float spd = g_doorSpeed;
    if      (g_doorVisLeft > targetLeft  + 4.0f) g_doorVisLeft  -= spd;
    else if (g_doorVisLeft < targetLeft  - 4.0f) g_doorVisLeft  += spd;

    if      (g_doorVisRight > targetRight + 4.0f) g_doorVisRight -= spd;
    else if (g_doorVisRight < targetRight - 4.0f) g_doorVisRight += spd;

    // Final clamp
    g_doorVisLeft  = std::max(g_leftMin,  std::min(g_leftMax,  g_doorVisLeft));
    g_doorVisRight = std::max(g_rightMax, std::min(g_rightMin, g_doorVisRight));
}

// ---------------------------------------------------------------------------
// Overlay rendering via UpdateLayeredWindow (per-pixel alpha, Wine-compatible)
// ---------------------------------------------------------------------------

static void PremultiplyAlpha(BYTE* pBits, int w, int h)
{
    // UpdateLayeredWindow with ULW_ALPHA requires premultiplied BGRA pixels
    DWORD* p = (DWORD*)pBits;
    for (int i = 0; i < w * h; ++i)
    {
        BYTE b =  p[i]        & 0xFF;
        BYTE g = (p[i] >>  8) & 0xFF;
        BYTE r = (p[i] >> 16) & 0xFF;
        BYTE a = (p[i] >> 24) & 0xFF;
        if (a == 0)
        {
            p[i] = 0;
        }
        else if (a < 255)
        {
            b = (BYTE)((int)b * a / 255);
            g = (BYTE)((int)g * a / 255);
            r = (BYTE)((int)r * a / 255);
            p[i] = ((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
        }
    }
}

static void RenderOverlay(HWND hwndOverlay, int winX, int winY, int winW, int winH)
{
    if (winW <= 0 || winH <= 0)
        return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    // 32-bit top-down DIB for per-pixel alpha
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = winW;
    bmi.bmiHeader.biHeight      = -winH;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE* pBits   = NULL;
    HBITMAP hBmp  = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
    if (!hBmp)
    {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }

    ZeroMemory(pBits, winW * winH * 4);

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);

    // Draw images using GDI+
    // The Bitmap wraps the DIB bits as PixelFormat32bppARGB.
    // GDI stores BGRA, GDI+ PixelFormat32bppARGB is also BGRA in memory.
    {
        Gdiplus::Bitmap canvas(winW, winH, winW * 4, PixelFormat32bppARGB, pBits);
        Graphics gfx(&canvas);
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        // Bezel (static, centred)
        if (g_pDoorsBezel)
        {
            float bx = (float)winW / 2.0f - (float)g_dbW / 2.0f;
            float by = (float)winH / 2.0f - (float)g_dbH / 2.0f;
            gfx.DrawImage(g_pDoorsBezel, (int)bx, (int)by, g_dbW, g_dbH);
        }

        // Recalculate door range in case window was resized
        InitDoorRange(winW);

        // Left door: g_doorVisLeft is the centre-x, y is vertically centred
        float leftTopX = g_doorVisLeft  - (float)g_dlW / 2.0f;
        float leftTopY = (float)winH / 2.0f - (float)g_dlH / 2.0f;

        // Right door: g_doorVisRight is the centre-x, y is vertically centred
        float rightTopX = g_doorVisRight - (float)g_drW / 2.0f;
        float rightTopY = (float)winH / 2.0f - (float)g_drH / 2.0f;

        if (g_pDoorLeft)
            gfx.DrawImage(g_pDoorLeft,  (int)leftTopX,  (int)leftTopY,  g_dlW, g_dlH);
        if (g_pDoorRight)
            gfx.DrawImage(g_pDoorRight, (int)rightTopX, (int)rightTopY, g_drW, g_drH);
    }

    // Premultiply alpha for UpdateLayeredWindow
    PremultiplyAlpha(pBits, winW, winH);

    // Update the layered window
    POINT ptSrc  = { 0, 0 };
    POINT ptDest = { winX, winY };
    SIZE  sz     = { winW, winH };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(hwndOverlay, hdcScreen, &ptDest, &sz, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hOld);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// ---------------------------------------------------------------------------
// Overlay window
// ---------------------------------------------------------------------------

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateOverlayWindow()
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = g_hInstance;
    wc.lpszClassName = L"EADPOverlay";
    RegisterClassExW(&wc);

    return CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        L"EADPOverlay", NULL,
        WS_POPUP,
        0, 0, 1, 1,
        NULL, NULL, g_hInstance, NULL);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static PROCESS_INFORMATION g_pi = {};

static bool LaunchGame(const wchar_t* exePath)
{
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, exePath);
    PathRemoveFileSpecW(dir);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);

    return CreateProcessW(exePath, NULL, NULL, NULL, FALSE,
        0, NULL, dir, &si, &g_pi) != FALSE;
}

// --- Flexible game-window search (4 strategies) ----------------------------

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

static DWORD PidFromExeName(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe))
        do { if (_wcsicmp(pe.szExeFile, exeName) == 0) { found = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

static HWND FindGameWindow(DWORD launchedPid)
{
    // 1. Exact match: class + OpenParrot-patched title
    HWND hw = FindWindowA(EADP_GAME_CLASS, EADP_GAME_TITLE);
    if (hw) return hw;
    // 2. Class-only: game running without OpenParrot renaming the window
    hw = FindWindowA(EADP_GAME_CLASS, NULL);
    if (hw) return hw;
    // 3. Any visible window of the process we launched
    if (launchedPid) { hw = WindowFromPid(launchedPid); if (hw) return hw; }
    // 4. Scan for game.exe and find its window
    DWORD gamePid = PidFromExeName(L"game.exe");
    if (gamePid) return WindowFromPid(gamePid);
    return NULL;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    g_hInstance = hInstance;

    // GDI+ startup
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, NULL);

    // Load door images
    g_pDoorLeft  = LoadPNG(L"DoorLeft.png");
    g_pDoorRight = LoadPNG(L"DoorRight.png");
    g_pDoorsBezel = LoadPNG(L"DoorsBezel.png");  // optional

    if (!g_pDoorLeft || !g_pDoorRight)
    {
        MessageBoxW(NULL,
            L"DoorLeft.png or DoorRight.png not found next to EADPDoorOverlay.exe.",
            L"EADP Door Overlay", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }

    g_dlW = g_pDoorLeft->GetWidth();
    g_dlH = g_pDoorLeft->GetHeight();
    g_drW = g_pDoorRight->GetWidth();
    g_drH = g_pDoorRight->GetHeight();
    if (g_pDoorsBezel)
    {
        g_dbW = g_pDoorsBezel->GetWidth();
        g_dbH = g_pDoorsBezel->GetHeight();
    }

    // Optionally launch the game
    if (lpCmdLine && lpCmdLine[0] != '\0')
    {
        // Convert to wide
        wchar_t widePath[MAX_PATH] = {};
        MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, widePath, MAX_PATH);
        // Strip surrounding quotes if present
        wchar_t* p = widePath;
        if (*p == L'"') { ++p; wchar_t* q = wcsrchr(p, L'"'); if (q) *q = 0; }
        LaunchGame(p);
    }

    // Wait for the game window (up to 120 s; tries 4 strategies)
    HWND hwndGame = NULL;
    for (int i = 0; i < 1200 && !hwndGame; ++i)
    {
        hwndGame = FindGameWindow(g_pi.dwProcessId);
        if (!hwndGame) Sleep(100);
    }
    if (!hwndGame)
    {
        MessageBoxW(NULL,
            L"Game window not found.\n\n"
            L"Searched for class \"Eva\" (with and without OpenParrot title)\n"
            L"and for a running process named game.exe.\n\n"
            L"Make sure the game is running.",
            L"EADP Door Overlay", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }

    // Inject the hook DLL
    DWORD pid = 0;
    GetWindowThreadProcessId(hwndGame, &pid);
    if (!pid) pid = PidFromExeName(L"game.exe");

    // Keep a process handle for the liveness check in the render loop
    HANDLE hGameProc = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;

    if (!InjectDLL(pid))
    {
        MessageBoxW(NULL,
            L"Failed to inject EADPDoorHook.dll.\nMake sure it is next to EADPDoorOverlay.exe.",
            L"EADP Door Overlay", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }

    // Open shared memory (hook DLL creates it on load)
    if (!OpenSharedMemory())
    {
        MessageBoxW(NULL,
            L"Could not open shared memory from EADPDoorHook.dll.",
            L"EADP Door Overlay", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }

    // Create the transparent overlay window
    g_hwndOverlay = CreateOverlayWindow();
    ShowWindow(g_hwndOverlay, SW_SHOW);

    // Main loop: pump messages and update overlay at ~60 fps
    MSG msg = {};
    DWORD lastTick = GetTickCount();

    while (true)
    {
        // Drain message queue
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                goto cleanup;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Stop when the game process exits
        if (hGameProc && WaitForSingleObject(hGameProc, 0) != WAIT_TIMEOUT)
            break;

        DWORD now = GetTickCount();
        if (now - lastTick < 16)
        {
            Sleep(1);
            continue;
        }
        lastTick = now;

        // Re-find the window each frame (title may change, window may be recreated)
        RECT r;
        HWND hwndNow = FindGameWindow(pid);
        if (!hwndNow || !GetWindowRect(hwndNow, &r))
            continue;

        int winX = r.left;
        int winY = r.top;
        int winW = r.right  - r.left;
        int winH = r.bottom - r.top;

        StepDoorAnimation();
        RenderOverlay(g_hwndOverlay, winX, winY, winW, winH);
    }

cleanup:
    if (hGameProc) CloseHandle(hGameProc);
    if (g_pState)   { UnmapViewOfFile(g_pState);    g_pState    = NULL; }
    if (g_hSharedMem) { CloseHandle(g_hSharedMem);  g_hSharedMem = NULL; }

    delete g_pDoorLeft;
    delete g_pDoorRight;
    delete g_pDoorsBezel;

    Gdiplus::GdiplusShutdown(gdipToken);

    if (g_pi.hProcess)
    {
        CloseHandle(g_pi.hProcess);
        CloseHandle(g_pi.hThread);
    }

    return 0;
}

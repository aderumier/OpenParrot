// EADPDoorOverlay.exe – single standalone exe, no DLL injection.
//
// Attaches to the running Elevator Action Death Parade game as a debugger
// and plants two hardware execution breakpoints:
//   imageBase + 0xC4B10  AttractionDoor(float left, float right, float unk)
//   imageBase + 0xC4C40  VibrationDoor (float unk,  int power,   int time)
//
// When either function is called the CPU fires EXCEPTION_SINGLE_STEP.
// We read the float parameters off the game's stack with ReadProcessMemory,
// update shared state, clear DR6 / set RF so execution continues normally,
// then render DoorLeft.png / DoorRight.png / DoorsBezel.png as a transparent
// layered window on top of the game using GDI+ + UpdateLayeredWindow.
//
// Must be compiled as x86 (32-bit) – the game is 32-bit and we read its stack.
//
// Images expected next to this exe:
//   DoorLeft.png    left door panel
//   DoorRight.png   right door panel
//   DoorsBezel.png  static bezel behind the doors (optional)
//
// Usage:
//   EADPDoorOverlay.exe               attach to already-running game
//   EADPDoorOverlay.exe "game.exe"    launch game then attach

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "kernel32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define GAME_CLASS         "Eva"
#define GAME_TITLE         "OpenParrot - Elevator Action: Death Parade"
#define ATTR_DOOR_OFFSET   0xC4B10u   // AttractionDoor  (cdecl float,float,float)
#define VIB_DOOR_OFFSET    0xC4C40u   // VibrationDoor   (cdecl float,int,int)

// ---------------------------------------------------------------------------
// State written by debug thread, read by render thread.
// 4-byte aligned volatile floats/LONGs are atomically readable on x86.
// ---------------------------------------------------------------------------
static volatile HANDLE g_hGameProc  = NULL;
static volatile DWORD  g_imageBase  = 0;

static volatile float  g_attrLeft   = 0.0f;  // door target: 0=open 1=closed
static volatile float  g_attrRight  = 0.0f;
static volatile LONG   g_vibPower   = 0;
static volatile LONG   g_vibTime    = 0;
static volatile LONG   g_vibGen     = 0;      // incremented on each vibration event

// ---------------------------------------------------------------------------
// Hardware breakpoint helpers
// ---------------------------------------------------------------------------

static void SetHWBP(HANDLE hThread, DWORD base)
{
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(hThread, &ctx))
        return;
    ctx.Dr0 = base + ATTR_DOOR_OFFSET;
    ctx.Dr1 = base + VIB_DOOR_OFFSET;
    // DR7: L0=bit0, L1=bit2 → 0x5; conditions 00=execute, length 00=1-byte (all zero)
    ctx.Dr7 = 0x00000005u;
    SetThreadContext(hThread, &ctx);
}

// ---------------------------------------------------------------------------
// Debug event loop (dedicated thread)
// ---------------------------------------------------------------------------

static DWORD WINAPI DebugLoop(LPVOID pPid)
{
    DWORD pid = (DWORD)(ULONG_PTR)pPid;

    // Open a separate VM-read handle; we keep it for ReadProcessMemory.
    g_hGameProc = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if (!g_hGameProc)
        return 1;

    if (!DebugActiveProcess(pid))
    {
        CloseHandle(g_hGameProc);
        g_hGameProc = NULL;
        return 1;
    }

    // Don't kill the game when our overlay exits.
    DebugSetProcessKillOnExit(FALSE);

    bool initBPDone = false;

    for (;;)
    {
        DEBUG_EVENT de = {};
        if (!WaitForDebugEvent(&de, 500))
        {
            // Timeout – bail if the game exited.
            if (WaitForSingleObject((HANDLE)g_hGameProc, 0) != WAIT_TIMEOUT)
                break;
            continue;
        }

        DWORD cont = DBG_CONTINUE;

        switch (de.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
        {
            DWORD base = (DWORD)(ULONG_PTR)de.u.CreateProcessInfo.lpBaseOfImage;
            g_imageBase = base;
            SetHWBP(de.u.CreateProcessInfo.hThread, base);
            if (de.u.CreateProcessInfo.hFile)
                CloseHandle(de.u.CreateProcessInfo.hFile);
            break;
        }

        case CREATE_THREAD_DEBUG_EVENT:
        {
            // Arm every new thread so whichever thread calls the door
            // function gets caught.
            DWORD base = g_imageBase;
            if (base)
            {
                HANDLE ht = OpenThread(
                    THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, de.dwThreadId);
                if (ht) { SetHWBP(ht, base); CloseHandle(ht); }
            }
            break;
        }

        case LOAD_DLL_DEBUG_EVENT:
            if (de.u.LoadDll.hFile)
                CloseHandle(de.u.LoadDll.hFile);
            break;

        case EXCEPTION_DEBUG_EVENT:
        {
            auto& er = de.u.Exception.ExceptionRecord;

            if (er.ExceptionCode == EXCEPTION_SINGLE_STEP)
            {
                DWORD addr = (DWORD)(ULONG_PTR)er.ExceptionAddress;
                DWORD base = g_imageBase;
                HANDLE hp  = (HANDLE)g_hGameProc;

                HANDLE ht = OpenThread(
                    THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, de.dwThreadId);

                if (ht && base && hp)
                {
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_FULL;
                    GetThreadContext(ht, &ctx);

                    if (addr == base + ATTR_DOOR_OFFSET)
                    {
                        // __cdecl: [ESP+0]=retaddr, [ESP+4]=left, [ESP+8]=right
                        float l = 0.0f, r = 0.0f;
                        ReadProcessMemory(hp, (LPCVOID)(ctx.Esp + 4), &l, 4, NULL);
                        ReadProcessMemory(hp, (LPCVOID)(ctx.Esp + 8), &r, 4, NULL);
                        g_attrLeft  = l;
                        g_attrRight = r;
                    }
                    else if (addr == base + VIB_DOOR_OFFSET)
                    {
                        // __cdecl: [ESP+4]=float, [ESP+8]=power, [ESP+12]=time
                        LONG pw = 0, tm = 0;
                        ReadProcessMemory(hp, (LPCVOID)(ctx.Esp + 8),  &pw, 4, NULL);
                        ReadProcessMemory(hp, (LPCVOID)(ctx.Esp + 12), &tm, 4, NULL);
                        InterlockedExchange(&g_vibPower, pw);
                        InterlockedExchange(&g_vibTime,  tm);
                        InterlockedIncrement(&g_vibGen);
                    }

                    // Clear DR6 status bits and set RF so the breakpointed
                    // instruction executes once without re-firing the exception.
                    ctx.Dr6     = 0;
                    ctx.EFlags |= 0x00010000u;  // RF (Resume Flag)
                    SetThreadContext(ht, &ctx);
                }
                if (ht) CloseHandle(ht);
            }
            else if (er.ExceptionCode == EXCEPTION_BREAKPOINT && !initBPDone)
            {
                initBPDone = true;  // initial attach-breakpoint; swallow it
            }
            else
            {
                cont = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        }

        case EXIT_PROCESS_DEBUG_EVENT:
            ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_CONTINUE);
            goto done;
        }

        ContinueDebugEvent(de.dwProcessId, de.dwThreadId, cont);
    }

done:
    CloseHandle((HANDLE)g_hGameProc);
    g_hGameProc = NULL;
    return 0;
}

// ---------------------------------------------------------------------------
// Door animation state (render thread only)
// ---------------------------------------------------------------------------

static float g_doorVisLeft  = 0.0f;
static float g_doorVisRight = 0.0f;
static bool  g_doorRangeInit = false;

static float g_leftMin  = 0.0f, g_leftMax  = 0.0f;
static float g_rightMin = 0.0f, g_rightMax = 0.0f;
static float g_doorSpeed = 8.0f;

static float g_doorFloatLeft  = 0.0f;
static float g_doorFloatRight = 0.0f;
static float g_savedLeft      = 0.0f;
static float g_savedRight     = 0.0f;

static bool  g_vibActive      = false;
static int   g_vibCount       = 0;
static int   g_vibSleep       = 0;
static bool  g_vibPhase       = false;
static LONG  g_lastVibGen     = 0;

static int g_dlW = 0, g_dlH = 0;
static int g_drW = 0, g_drH = 0;

static void InitDoorRange(int winW)
{
    float cx = (float)(int)round((float)winW / 2.0f);
    g_leftMin  = cx - (float)g_dlW * 2.0f + (float)g_dlW / 2.0f;
    g_leftMax  = cx - (float)g_dlW / 2.0f;
    g_rightMax = cx + (float)g_drW / 2.0f;
    g_rightMin = cx + (float)g_drW * 2.0f - (float)g_drW / 2.0f;
    if (!g_doorRangeInit)
    {
        g_doorVisLeft  = g_leftMin;
        g_doorVisRight = g_rightMin;
        g_doorRangeInit = true;
    }
    g_doorSpeed = ((float)g_dlW / 2.0f) / 405.0f * 8.0f;
    if (g_doorSpeed < 1.0f) g_doorSpeed = 1.0f;
}

static void StepDoorAnimation(int winW)
{
    InitDoorRange(winW);

    // Pick up latest attraction values when not vibrating
    if (!g_vibActive)
    {
        g_doorFloatLeft  = g_attrLeft;
        g_doorFloatRight = g_attrRight;
        g_savedLeft      = g_doorFloatLeft;
        g_savedRight     = g_doorFloatRight;
    }

    // Detect new vibration trigger
    LONG curGen = InterlockedCompareExchange(&g_vibGen, 0, 0);
    if (curGen != g_lastVibGen && !g_vibActive)
    {
        g_lastVibGen = curGen;
        g_vibActive  = true;
        g_vibCount   = 0;
        g_vibSleep   = 0;
        g_vibPhase   = false;
    }

    if (g_vibActive)
    {
        ++g_vibCount;
        ++g_vibSleep;
        float power = (float)(int)InterlockedCompareExchange(&g_vibPower, 0, 0) / 200.0f;
        if (g_vibSleep >= 2) { g_vibSleep = 0; g_vibPhase = !g_vibPhase; }
        if (g_vibPhase) { g_doorFloatLeft += power; g_doorFloatRight += power; }
        else            { g_doorFloatLeft -= power; g_doorFloatRight -= power; }
        g_doorFloatLeft  = std::max(0.0f, std::min(1.0f, g_doorFloatLeft));
        g_doorFloatRight = std::max(0.0f, std::min(1.0f, g_doorFloatRight));

        int vt = (int)InterlockedCompareExchange(&g_vibTime, 0, 0);
        if (g_vibCount >= (int)(vt / 16.0f))
        {
            g_vibActive      = false;
            g_doorFloatLeft  = g_savedLeft;
            g_doorFloatRight = g_savedRight;
        }
    }

    // Slide visual positions toward targets
    float leftTotal  = g_leftMax  - g_leftMin;
    float rightTotal = g_rightMin - g_rightMax;
    float targetLeft  = g_doorFloatLeft  * leftTotal  + g_leftMin;
    float targetRight = g_rightMin - g_doorFloatRight * rightTotal;

    g_doorVisLeft  = std::max(g_leftMin,  std::min(g_leftMax,  g_doorVisLeft));
    g_doorVisRight = std::max(g_rightMax, std::min(g_rightMin, g_doorVisRight));

    float spd = g_doorSpeed;
    if      (g_doorVisLeft  > targetLeft  + 4.0f) g_doorVisLeft  -= spd;
    else if (g_doorVisLeft  < targetLeft  - 4.0f) g_doorVisLeft  += spd;
    if      (g_doorVisRight > targetRight + 4.0f) g_doorVisRight -= spd;
    else if (g_doorVisRight < targetRight - 4.0f) g_doorVisRight += spd;

    g_doorVisLeft  = std::max(g_leftMin,  std::min(g_leftMax,  g_doorVisLeft));
    g_doorVisRight = std::max(g_rightMax, std::min(g_rightMin, g_doorVisRight));
}

// ---------------------------------------------------------------------------
// GDI+ overlay rendering
// ---------------------------------------------------------------------------

static void PremultiplyAlpha(BYTE* pBits, int w, int h)
{
    DWORD* p = (DWORD*)pBits;
    for (int i = 0; i < w * h; ++i)
    {
        BYTE b =  p[i]        & 0xFF;
        BYTE g = (p[i] >>  8) & 0xFF;
        BYTE r = (p[i] >> 16) & 0xFF;
        BYTE a = (p[i] >> 24) & 0xFF;
        if (a == 0)          { p[i] = 0; continue; }
        if (a == 255)        continue;
        b = (BYTE)((int)b * a / 255);
        g = (BYTE)((int)g * a / 255);
        r = (BYTE)((int)r * a / 255);
        p[i] = ((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
    }
}

static void RenderOverlay(
    HWND hwnd,
    int winX, int winY, int winW, int winH,
    Gdiplus::Image* pLeft, Gdiplus::Image* pRight, Gdiplus::Image* pBezel,
    int dbW, int dbH)
{
    if (winW <= 0 || winH <= 0) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = winW;
    bmi.bmiHeader.biHeight      = -winH;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE*   pBits = NULL;
    HBITMAP hBmp  = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
    if (!hBmp) { DeleteDC(hdcMem); ReleaseDC(NULL, hdcScreen); return; }
    ZeroMemory(pBits, (size_t)winW * winH * 4);

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);

    {
        // Wrap the DIB bits as a GDI+ bitmap (BGRA == PixelFormat32bppARGB in memory)
        Gdiplus::Bitmap canvas(winW, winH, winW * 4, PixelFormat32bppARGB, pBits);
        Gdiplus::Graphics gfx(&canvas);
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        if (pBezel)
        {
            int bx = winW / 2 - dbW / 2;
            int by = winH / 2 - dbH / 2;
            gfx.DrawImage(pBezel, bx, by, dbW, dbH);
        }

        // Centre-based → top-left
        int leftX  = (int)g_doorVisLeft  - g_dlW / 2;
        int leftY  = winH / 2            - g_dlH / 2;
        int rightX = (int)g_doorVisRight - g_drW / 2;
        int rightY = winH / 2            - g_drH / 2;

        if (pLeft)  gfx.DrawImage(pLeft,  leftX,  leftY,  g_dlW, g_dlH);
        if (pRight) gfx.DrawImage(pRight, rightX, rightY, g_drW, g_drH);
    }

    PremultiplyAlpha(pBits, winW, winH);

    POINT ptSrc  = { 0, 0 };
    POINT ptDest = { winX, winY };
    SIZE  sz     = { winW, winH };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, hdcScreen, &ptDest, &sz, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hOld);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// ---------------------------------------------------------------------------
// Overlay window
// ---------------------------------------------------------------------------

static HINSTANCE g_hInst = NULL;

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateOverlay()
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = L"EADPOverlay";
    RegisterClassExW(&wc);
    return CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        L"EADPOverlay", NULL, WS_POPUP,
        0, 0, 1, 1, NULL, NULL, g_hInst, NULL);
}

// ---------------------------------------------------------------------------
// Image loader
// ---------------------------------------------------------------------------

static std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

static Gdiplus::Bitmap* LoadPNG(const wchar_t* name)
{
    std::wstring path = ExeDir() + L"\\" + name;
    auto* bmp = Gdiplus::Bitmap::FromFile(path.c_str());
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return NULL; }
    return bmp;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

static PROCESS_INFORMATION g_pi = {};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    g_hInst = hInstance;

    // GDI+ init
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken;
    Gdiplus::GdiplusStartup(&gdipToken, &gsi, NULL);

    // Load images
    auto* pLeft  = LoadPNG(L"DoorLeft.png");
    auto* pRight = LoadPNG(L"DoorRight.png");
    auto* pBezel = LoadPNG(L"DoorsBezel.png");  // optional

    if (!pLeft || !pRight)
    {
        MessageBoxW(NULL,
            L"DoorLeft.png or DoorRight.png not found next to EADPDoorOverlay.exe.",
            L"EADP Door Overlay", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }

    g_dlW = (int)pLeft->GetWidth();   g_dlH = (int)pLeft->GetHeight();
    g_drW = (int)pRight->GetWidth();  g_drH = (int)pRight->GetHeight();
    int dbW = pBezel ? (int)pBezel->GetWidth()  : 0;
    int dbH = pBezel ? (int)pBezel->GetHeight() : 0;

    // Optionally launch the game
    if (lpCmdLine && lpCmdLine[0])
    {
        wchar_t wide[MAX_PATH] = {};
        MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, wide, MAX_PATH);
        wchar_t* p = wide;
        if (*p == L'"') { ++p; wchar_t* q = wcsrchr(p, L'"'); if (q) *q = 0; }

        wchar_t dir[MAX_PATH];
        wcscpy_s(dir, p);
        PathRemoveFileSpecW(dir);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        CreateProcessW(p, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &g_pi);
    }

    // Wait for the game window
    HWND hwndGame = NULL;
    for (int i = 0; i < 600 && !hwndGame; ++i)
    {
        hwndGame = FindWindowA(GAME_CLASS, GAME_TITLE);
        if (!hwndGame) Sleep(100);
    }
    if (!hwndGame)
    {
        MessageBoxW(NULL, L"Game window not found.", L"EADP Door Overlay", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwndGame, &pid);

    // Start debug loop thread – this attaches to the game and arms the breakpoints
    HANDLE hDbgThread = CreateThread(NULL, 0, DebugLoop, (LPVOID)(ULONG_PTR)pid, 0, NULL);
    if (!hDbgThread)
    {
        MessageBoxW(NULL, L"Failed to start debug thread.", L"EADP Door Overlay", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }

    // Wait until DebugLoop has attached and set g_imageBase (max ~3 s)
    for (int i = 0; i < 150 && !g_imageBase; ++i) Sleep(20);

    if (!g_imageBase)
    {
        MessageBoxW(NULL,
            L"Could not attach debugger to game.\n"
            L"Make sure no other debugger is attached.",
            L"EADP Door Overlay", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }

    // Overlay window
    HWND hwndOverlay = CreateOverlay();
    ShowWindow(hwndOverlay, SW_SHOW);

    // Main render loop (~60 fps)
    MSG  msg   = {};
    DWORD last = GetTickCount();

    for (;;)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!FindWindowA(GAME_CLASS, GAME_TITLE)) break;

        DWORD now = GetTickCount();
        if (now - last < 16) { Sleep(1); continue; }
        last = now;

        RECT r;
        HWND hwndNow = FindWindowA(GAME_CLASS, GAME_TITLE);
        if (!hwndNow) break;
        if (!GetWindowRect(hwndNow, &r)) continue;

        int winW = r.right - r.left;
        int winH = r.bottom - r.top;

        StepDoorAnimation(winW);
        RenderOverlay(hwndOverlay,
            r.left, r.top, winW, winH,
            pLeft, pRight, pBezel, dbW, dbH);
    }

cleanup:
    DebugActiveProcessStop(pid);
    WaitForSingleObject(hDbgThread, 3000);
    CloseHandle(hDbgThread);

    delete pLeft;
    delete pRight;
    delete pBezel;

    Gdiplus::GdiplusShutdown(gdipToken);

    if (g_pi.hProcess) { CloseHandle(g_pi.hProcess); CloseHandle(g_pi.hThread); }
    return 0;
}

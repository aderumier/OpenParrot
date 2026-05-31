// EADPDoorHook.dll – self-contained 32-bit DLL for EADP door overlay.
//
// Hooks two game functions to capture door state:
//   imageBase + 0xC4B10  AttractionDoor(float left, float right, float unk)
//   imageBase + 0xC4C40  VibrationDoor (float unk,  int power,   int time)
//
// Hooks IDirect3DDevice9::EndScene via vtable to render the door images
// directly into the game's D3D9 backbuffer (no separate overlay window).
//
// PNG images expected next to the game exe:
//   DoorLeft.png, DoorRight.png, DoorsBezel.png (optional)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>
#include <d3d9.h>
#include "DirectXSDK/Include/d3dx9.h"
#include "MinHook.h"
#include <algorithm>
#include <cmath>

#pragma comment(lib, "shlwapi.lib")

// ---------------------------------------------------------------------------
// Door state – written by game-function hooks, read by EndScene
// ---------------------------------------------------------------------------
static volatile float g_attrLeft  = 0.0f;
static volatile float g_attrRight = 0.0f;
static volatile LONG  g_vibPower  = 0;
static volatile LONG  g_vibTime   = 0;
static volatile LONG  g_vibGen    = 0;   // incremented on each vibration event

// ---------------------------------------------------------------------------
// Door animation state – owned by the EndScene thread
// ---------------------------------------------------------------------------
static float g_doorFloatLeft  = 0.0f;
static float g_doorFloatRight = 0.0f;
static float g_savedLeft      = 0.0f;
static float g_savedRight     = 0.0f;
static float g_doorVisLeft    = 0.0f;
static float g_doorVisRight   = 0.0f;
static bool  g_doorRangeInit  = false;
static float g_leftMin = 0, g_leftMax = 0, g_rightMin = 0, g_rightMax = 0;
static float g_doorSpeed      = 8.0f;

static bool  g_vibActive      = false;
static int   g_vibCount       = 0;
static int   g_vibSleep       = 0;
static bool  g_vibPhase       = false;
static LONG  g_lastVibGen     = 0;

// ---------------------------------------------------------------------------
// D3D9 rendering state – created on first EndScene call
// ---------------------------------------------------------------------------
static bool               g_renderInit  = false;
static LPDIRECT3DTEXTURE9 g_texLeft     = NULL;
static LPDIRECT3DTEXTURE9 g_texRight    = NULL;
static LPDIRECT3DTEXTURE9 g_texBezel    = NULL;
static LPD3DXSPRITE       g_sprLeft     = NULL;
static LPD3DXSPRITE       g_sprRight    = NULL;
static LPD3DXSPRITE       g_sprBezel    = NULL;
static UINT g_dlW = 0, g_dlH = 0;
static UINT g_drW = 0, g_drH = 0;
static UINT g_dbW = 0, g_dbH = 0;

// ---------------------------------------------------------------------------
// Game function hooks
// ---------------------------------------------------------------------------
typedef int (__cdecl* AttractionDoor_t)(float, float, float);
typedef int (__cdecl* VibrationDoor_t)(float, int, int);
static AttractionDoor_t g_AttrOri = NULL;
static VibrationDoor_t  g_VibOri  = NULL;

static int __cdecl AttractionDoorHook(float a1, float a2, float a3)
{
    g_attrLeft  = a1;
    g_attrRight = a2;
    return g_AttrOri(a1, a2, a3);
}

static int __cdecl VibrationDoorHook(float a1, int a2, int a3)
{
    InterlockedExchange(&g_vibPower, a2);
    InterlockedExchange(&g_vibTime,  a3);
    InterlockedIncrement(&g_vibGen);
    return g_VibOri(a1, a2, a3);
}

// ---------------------------------------------------------------------------
// Door position + animation (mirrors D3D9Misc.cpp EADPAttractionDoorsCalculations)
// ---------------------------------------------------------------------------
static void UpdateDoorRange(UINT renderW)
{
    float cx = (float)(int)round((float)renderW / 2.0f);
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

static void StepDoorAnimation(UINT renderW)
{
    UpdateDoorRange(renderW);

    if (!g_vibActive)
    {
        g_doorFloatLeft  = g_attrLeft;
        g_doorFloatRight = g_attrRight;
        g_savedLeft      = g_doorFloatLeft;
        g_savedRight     = g_doorFloatRight;
    }

    LONG cur = InterlockedCompareExchange(&g_vibGen, 0, 0);
    if (cur != g_lastVibGen && !g_vibActive)
    {
        g_lastVibGen = cur;
        g_vibActive  = true;
        g_vibCount = g_vibSleep = 0;
        g_vibPhase = false;
    }

    if (g_vibActive)
    {
        ++g_vibCount; ++g_vibSleep;
        float pw = (float)(int)InterlockedCompareExchange(&g_vibPower, 0, 0) / 200.0f;
        if (g_vibSleep >= 2) { g_vibSleep = 0; g_vibPhase = !g_vibPhase; }
        if (g_vibPhase) { g_doorFloatLeft += pw; g_doorFloatRight += pw; }
        else            { g_doorFloatLeft -= pw; g_doorFloatRight -= pw; }
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

    float leftTotal  = g_leftMax  - g_leftMin;
    float rightTotal = g_rightMin - g_rightMax;
    float tL = g_doorFloatLeft  * leftTotal  + g_leftMin;
    float tR = g_rightMin - g_doorFloatRight * rightTotal;

    g_doorVisLeft  = std::max(g_leftMin,  std::min(g_leftMax,  g_doorVisLeft));
    g_doorVisRight = std::max(g_rightMax, std::min(g_rightMin, g_doorVisRight));

    float spd = g_doorSpeed;
    if      (g_doorVisLeft  > tL + 4.0f) g_doorVisLeft  -= spd;
    else if (g_doorVisLeft  < tL - 4.0f) g_doorVisLeft  += spd;
    if      (g_doorVisRight > tR + 4.0f) g_doorVisRight -= spd;
    else if (g_doorVisRight < tR - 4.0f) g_doorVisRight += spd;

    g_doorVisLeft  = std::max(g_leftMin,  std::min(g_leftMax,  g_doorVisLeft));
    g_doorVisRight = std::max(g_rightMax, std::min(g_rightMin, g_doorVisRight));
}

// ---------------------------------------------------------------------------
// D3D9 resource init (called once on first EndScene)
// ---------------------------------------------------------------------------
static void InitRenderResources(IDirect3DDevice9* dev)
{
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    PathRemoveFileSpecW(dir);

    auto tryLoad = [&](const wchar_t* name,
                       LPDIRECT3DTEXTURE9& tex, LPD3DXSPRITE& spr,
                       UINT& w, UINT& h)
    {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%s", dir, name);
        if (SUCCEEDED(D3DXCreateTextureFromFileExW(dev, path,
            D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT,
            0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
            D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &tex)))
        {
            D3DXCreateSprite(dev, &spr);
            D3DSURFACE_DESC desc;
            tex->GetLevelDesc(0, &desc);
            w = desc.Width;
            h = desc.Height;
        }
    };

    tryLoad(L"DoorLeft.png",   g_texLeft,  g_sprLeft,  g_dlW, g_dlH);
    tryLoad(L"DoorRight.png",  g_texRight, g_sprRight, g_drW, g_drH);
    tryLoad(L"DoorsBezel.png", g_texBezel, g_sprBezel, g_dbW, g_dbH);
    g_renderInit = true;
}

// ---------------------------------------------------------------------------
// EndScene hook
// ---------------------------------------------------------------------------
typedef HRESULT (APIENTRY* EndScene_t)(IDirect3DDevice9*);
static EndScene_t g_EndSceneOri = NULL;

static HRESULT APIENTRY EndScene_hook(IDirect3DDevice9* dev)
{
    if (!g_renderInit)
        InitRenderResources(dev);

    if (g_dlW && g_drW)
    {
        UINT renderW = 1280, renderH = 720;
        IDirect3DSurface9* pBack = NULL;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBack)) && pBack)
        {
            D3DSURFACE_DESC desc;
            pBack->GetDesc(&desc);
            renderW = desc.Width;
            renderH = desc.Height;
            pBack->Release();
        }

        StepDoorAnimation(renderW);

        float cy = (float)renderH / 2.0f;

        auto draw = [&](LPD3DXSPRITE spr, LPDIRECT3DTEXTURE9 tex,
                        UINT w, UINT h, float cx)
        {
            if (!spr || !tex) return;
            D3DXVECTOR3 cen((float)w / 2.0f, (float)h / 2.0f, 0.0f);
            D3DXVECTOR3 pos(cx, cy, 0.0f);
            spr->Begin(D3DXSPRITE_ALPHABLEND);
            spr->Draw(tex, NULL, &cen, &pos, 0xFFFFFFFF);
            spr->End();
        };

        draw(g_sprBezel, g_texBezel, g_dbW, g_dbH, (float)renderW / 2.0f);
        draw(g_sprLeft,  g_texLeft,  g_dlW, g_dlH, g_doorVisLeft);
        draw(g_sprRight, g_texRight, g_drW, g_drH, g_doorVisRight);
    }

    return g_EndSceneOri(dev);
}

// ---------------------------------------------------------------------------
// D3D9 vtable hook setup (background thread – waits for game window first)
// ---------------------------------------------------------------------------
static DWORD WINAPI HookD3D9(LPVOID)
{
    while (!FindWindowA("Eva", NULL)) Sleep(16);

    // Throw-away window so device creation doesn't touch the game window
    HWND hwndDummy = CreateWindowW(L"STATIC", NULL, WS_POPUP,
        0, 0, 1, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);

    IDirect3D9Ex* pD3D = NULL;
    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &pD3D))) goto done;

    {
        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed   = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;

        IDirect3DDevice9Ex* pDev = NULL;
        HRESULT hr = pD3D->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            hwndDummy, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &pDev);
        if (FAILED(hr))
            hr = pD3D->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF,
                hwndDummy, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &pDev);

        if (SUCCEEDED(hr) && pDev)
        {
            DWORD* vtable = *(DWORD**)pDev;
            // IDirect3DDevice9 vtable slot 42 = EndScene
            MH_CreateHook((void*)vtable[42], EndScene_hook, (void**)&g_EndSceneOri);
            MH_EnableHook((void*)vtable[42]);
            pDev->Release();
        }
        pD3D->Release();
    }

done:
    if (hwndDummy) DestroyWindow(hwndDummy);
    return 0;
}

// ---------------------------------------------------------------------------
// DLL entry point
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInstDLL);
        MH_Initialize();
        {
            DWORD base = (DWORD)GetModuleHandleA(NULL);
            MH_CreateHook((void*)(base + 0xC4B10), AttractionDoorHook, (void**)&g_AttrOri);
            MH_CreateHook((void*)(base + 0xC4C40), VibrationDoorHook,  (void**)&g_VibOri);
        }
        MH_EnableHook(MH_ALL_HOOKS);
        CreateThread(NULL, 0, HookD3D9, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_sprLeft)  { g_sprLeft->Release();  g_sprLeft  = NULL; }
        if (g_sprRight) { g_sprRight->Release(); g_sprRight = NULL; }
        if (g_sprBezel) { g_sprBezel->Release(); g_sprBezel = NULL; }
        if (g_texLeft)  { g_texLeft->Release();  g_texLeft  = NULL; }
        if (g_texRight) { g_texRight->Release(); g_texRight = NULL; }
        if (g_texBezel) { g_texBezel->Release(); g_texBezel = NULL; }
        break;
    }
    return TRUE;
}

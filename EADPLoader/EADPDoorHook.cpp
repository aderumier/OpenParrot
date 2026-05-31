// EADPDoorHook.dll
// 32-bit DLL injected into the Elevator Action: Death Parade game process.
// Hooks two game functions to capture door state and writes it to named shared memory
// so that EADPDoorOverlay.exe can render the door images.
//
// Hook offsets (relative to game image base, verified from OpenParrot source):
//   0xC4B10  AttractionDoor(float left, float right, float unk)
//   0xC4C40  VibrationDoor(float unk, int power, int time)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"
#include "EADPShared.h"

static HANDLE          g_hSharedMem = NULL;
static EADPSharedState* g_pState    = NULL;

typedef int (__cdecl* AttractionDoor_t)(float a1, float a2, float a3);
typedef int (__cdecl* VibrationDoor_t)(float a1, int a2, int a3);

static AttractionDoor_t g_AttractionDoorOri = NULL;
static VibrationDoor_t  g_VibrationDoorOri  = NULL;

static int __cdecl AttractionDoorHook(float a1, float a2, float a3)
{
    if (g_pState)
    {
        // Interlocked writes so the overlay always sees consistent values
        // (floats fit in a LONG on x86)
        InterlockedExchange((LONG*)&g_pState->AttractionDoorLeft,  *(LONG*)&a1);
        InterlockedExchange((LONG*)&g_pState->AttractionDoorRight, *(LONG*)&a2);
    }
    return g_AttractionDoorOri(a1, a2, a3);
}

static int __cdecl VibrationDoorHook(float a1, int a2, int a3)
{
    if (g_pState)
    {
        InterlockedExchange(&g_pState->VibrationPower, a2);
        InterlockedExchange(&g_pState->VibrationTime,  a3);
        // Set effect flag last so overlay sees power/time before it starts
        InterlockedExchange(&g_pState->VibrationEffect, 1L);
    }
    return g_VibrationDoorOri(a1, a2, a3);
}

static DWORD WINAPI InitThread(LPVOID)
{
    g_hSharedMem = CreateFileMappingW(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(EADPSharedState), EADP_SHARED_NAME);

    if (!g_hSharedMem)
        return 1;

    g_pState = (EADPSharedState*)MapViewOfFile(
        g_hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EADPSharedState));

    if (!g_pState)
        return 1;

    g_pState->AttractionDoorLeft  = 0.0f;
    g_pState->AttractionDoorRight = 0.0f;
    g_pState->VibrationPower      = 0;
    g_pState->VibrationTime       = 0;
    g_pState->VibrationEffect     = 0;

    DWORD imageBase = (DWORD)GetModuleHandleA(NULL);

    MH_Initialize();
    MH_CreateHook((void*)(imageBase + 0xC4B10), AttractionDoorHook, (void**)&g_AttractionDoorOri);
    MH_CreateHook((void*)(imageBase + 0xC4C40), VibrationDoorHook,  (void**)&g_VibrationDoorOri);
    MH_EnableHook(MH_ALL_HOOKS);

    return 0;
}

static void Uninit()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_pState)  { UnmapViewOfFile(g_pState);   g_pState    = NULL; }
    if (g_hSharedMem) { CloseHandle(g_hSharedMem); g_hSharedMem = NULL; }
}

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInstDLL);
        // Initialize in a separate thread so DllMain returns quickly
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        Uninit();
        break;
    }
    return TRUE;
}

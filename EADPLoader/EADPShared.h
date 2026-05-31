#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Named shared memory used between EADPDoorHook.dll (game process) and EADPDoorOverlay.exe
#define EADP_SHARED_NAME    L"Local\\EADPDoorState"
#define EADP_GAME_CLASS     "Eva"
#define EADP_GAME_TITLE     "OpenParrot - Elevator Action: Death Parade"
#define EADP_HOOK_DLL_NAME  L"EADPDoorHook.dll"

// Door float values: 0.0 = fully open, 1.0 = fully closed
#pragma pack(push, 1)
struct EADPSharedState
{
    float AttractionDoorLeft;   // target left door position (written by hook)
    float AttractionDoorRight;  // target right door position (written by hook)
    LONG  VibrationPower;       // vibration intensity
    LONG  VibrationTime;        // vibration duration (frames * 16)
    LONG  VibrationEffect;      // non-zero = vibration active (written by hook, cleared by overlay)
};
#pragma pack(pop)

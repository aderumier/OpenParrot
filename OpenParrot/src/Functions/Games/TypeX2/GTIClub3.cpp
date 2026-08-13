#include <StdInc.h>
#include "Utility/InitFunction.h"
#include "Functions/Global.h"
#include "Utility/WineCompat.h"
#if _M_IX86
DWORD mainModuleBase;

static bool IsGtiClubDiagnosticsEnabled()
{
	char value[8] = {};
	const DWORD length = GetEnvironmentVariableA(
		"TP_ANDROID_DEBUG_LOGGING",
		value,
		_countof(value));
	return length > 0 &&
		length < _countof(value) &&
		value[0] == '1';
}

static bool ShouldTolerateGtiClubRawSocketBindFailure()
{
	// No Wine host grants the game the raw-socket privileges AVS wants, so the
	// bind failure is expected on desktop Wine as well as under Winlator.
	if (IsWineCompatEnabled())
		return true;

	// Host-only validation switch. The production Wine path above does not
	// depend on this variable, but keeping the override allows the exact same
	// in-memory instruction to be exercised on Windows without changing the
	// executable or its CRC.
	char value[8] = {};
	const DWORD length = GetEnvironmentVariableA(
		"TP_GTI_TOLERATE_AVS_RAW_BIND_FAILURE",
		value,
		_countof(value));
	return length > 0 &&
		length < _countof(value) &&
		value[0] == '1';
}

static int __cdecl GtiClubAvsLogCallback(
	int,
	const char* message,
	int messageLength)
{
	if (!IsGtiClubDiagnosticsEnabled() ||
		message == nullptr ||
		messageLength <= 0)
		return 0;

	HANDLE logFile = CreateFileA(
		".\\GTIClub3AvsDiagnostic.log",
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (logFile != INVALID_HANDLE_VALUE)
	{
		DWORD bytesWritten = 0;
		WriteFile(
			logFile,
			message,
			static_cast<DWORD>(messageLength),
			&bytesWritten,
			nullptr);
		CloseHandle(logFile);
	}

	return 0;
}

int __cdecl IgnoreFunc(int a1, int a2)
{
	int result; // eax

	result = a2;
	*(BYTE *)(a2 + 8) = 1;

	// Enable Input
	*(BYTE *)(mainModuleBase + 0x67E75B) = 1;
	return result;
}

extern int* ffbOffset;
extern int* ffbOffset2;
extern int* ffbOffset3;
extern int* ffbOffset4;


float toFloat(uint8_t x)
{
	return x / 255.0;
}

float toFloatWheel(uint8_t x)
{
	return x / 255.0 * 2.0 - 1.0;
}

static int InjectKeys()
{
	int notButtons = ~*(WORD *)(mainModuleBase + 0x67E784);
	int buttons = *ffbOffset;
	DWORD wheel = *ffbOffset2;
	DWORD gas = *ffbOffset3;
	DWORD brake = *ffbOffset4;

	BYTE wheelVal = (wheel - 0xFF) / 0x100;
	BYTE gasVal = (gas - 0xFF) / 0x100;
	BYTE brakeVal = (brake - 0xFF) / 0x100;

	float wheelFloat = toFloatWheel(wheelVal);
	float gasFloat = toFloat(gasVal);
	float brakeFloat = toFloat(brakeVal);

	if (wheelVal < 0x60)
		buttons |= 0x200;

	if (wheelVal > 0xA0)
		buttons |= 0x400;

	if (gasVal > 0xA0)
		buttons |= 0x20;

	if (brakeVal > 0xA0)
		buttons |= 0x40;

	// Digital Inputs
	*(WORD *)(mainModuleBase + 0x67E784) = buttons;

	// Wheel Test Menu
	*(WORD *)(mainModuleBase + 0x518C8C) = *ffbOffset2;

	// Gas Test Menu
	*(WORD *)(mainModuleBase + 0x518C90) = *ffbOffset3;

	// Brake Test Menu
	*(WORD *)(mainModuleBase + 0x518C94) = *ffbOffset4;

	// Wheel Float
	memcpy((void *)(mainModuleBase + 0x518C80), &wheelFloat, 4);

	// Gas Float
	memcpy((void *)(mainModuleBase + 0x518C84), &gasFloat, 4);

	// Brake Float
	memcpy((void *)(mainModuleBase + 0x518C88), &brakeFloat, 4);

	*(DWORD*)0xA7E788 = buttons & notButtons;

	return 1;
}

static int(*g_origac_io_hbhi_COMUNICATE)();

signed int __cdecl ac_io_hbhi_COMUNICATE()
{
	InjectKeys();

	return g_origac_io_hbhi_COMUNICATE();
}

static InitFunction GtiClub3Func([]()
{

	mainModuleBase = (DWORD)GetModuleHandle(0);

	// EA3's keepalive layer creates a raw AVS socket successfully under Wine on
	// Android, but its bind returns 0x8007000D. The stock failure branch closes
	// that valid socket and intentionally aborts. Disabling AVS raw networking is
	// not safe: EA3 requests protocol ID 2 unconditionally and would instead
	// abort on an unregistered protocol. Keep the initialized socket and let the
	// title continue offline by skipping only the bind-failure cleanup branch.
	// libavs-win32-ea3.dll is a static game import, so it is loaded before this
	// title initializer. Validate the exact instruction bytes so other EA3 builds
	// and unknown revisions remain untouched.
	HMODULE ea3Module = GetModuleHandleA("libavs-win32-ea3.dll");
	const BYTE expectedBindFailureBranch[] =
		{ 0x0F, 0x8C, 0xE4, 0x04, 0x00, 0x00 };
	BYTE* bindFailureBranch = ea3Module
		? reinterpret_cast<BYTE*>(ea3Module) + 0x12958
		: nullptr;
	if (ShouldTolerateGtiClubRawSocketBindFailure() &&
		bindFailureBranch != nullptr &&
		memcmp(
			bindFailureBranch,
			expectedBindFailureBranch,
			sizeof(expectedBindFailureBranch)) == 0)
	{
		injector::MakeNOP(
			reinterpret_cast<DWORD>(bindFailureBranch),
			sizeof(expectedBindFailureBranch),
			true);
	}

	// The game's original AVS callback at this address discards every log
	// message, including the fatal message immediately before AVS invokes its
	// debugger-break abort handler. Preserve the production no-op behavior,
	// but retain those exact messages when the per-game Android diagnostics
	// toggle is enabled so Wine-only failures can be traced without guessing.
	injector::MakeJMP(
		mainModuleBase + 0x4DCB0,
		GtiClubAvsLogCallback,
		true);

	DWORD funcPtr = (DWORD)(void *)IgnoreFunc;

	CreateDirectoryA("EUP", nullptr);
	CreateDirectoryA("FUP", nullptr);

	injector::WriteMemory<DWORD>(mainModuleBase + 0x4C0E0C, funcPtr, true);
	injector::WriteMemory<DWORD>(mainModuleBase + 0x4C0E14, funcPtr, true);
	injector::WriteMemory<DWORD>(mainModuleBase + 0x4C0E1C, funcPtr, true);
	injector::WriteMemory<DWORD>(mainModuleBase + 0x4C0E3C, funcPtr, true);

	injector::WriteMemory<DWORD>(mainModuleBase + 0x357CB4, 0x55465C2E, true);
	injector::WriteMemory<DWORD>(mainModuleBase + 0x357C80, 0x55455C2E, true);

	MH_Initialize();
	MH_CreateHook((void*)(mainModuleBase + 0x409C0), ac_io_hbhi_COMUNICATE, (void**)&g_origac_io_hbhi_COMUNICATE); // ?
	MH_EnableHook(MH_ALL_HOOKS);

}, GameID::GTIClub3);
#endif

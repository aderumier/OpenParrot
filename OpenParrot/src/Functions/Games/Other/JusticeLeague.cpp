#include <StdInc.h>
#include "Utility/InitFunction.h"
#include "Functions/Global.h"
#include "Utility\Hooking.Patterns.h"
#include "Utility/WineCompat.h"
#include <Xinput.h>
#include <winbase.h>
#include <math.h>
#include <float.h>
#include <string>
#include <dinput.h>
#include "DirectXSDK/Include/d3dx9.h"

#pragma comment(lib, "Ws2_32.lib")
#if _M_IX86
#define clamp( x, xmin, xmax ) min( xmax, max( x, xmin ) )
typedef unsigned int U32;
typedef unsigned char U8;

static DWORD BaseAddress8 = 0x00400000;
static int horizontal8 = 0;
static int vertical8 = 0;
static HWND hWndRT8 = 0;

static bool previousLeft = false;
static bool previousRight = false;
static bool previousUp = false;
static bool previousDown = false;

static bool TESTpressed = false;
static bool COIN1pressed = false;
static bool COIN2pressed = false;

static DWORD justiceLeagueAspectGuardReturn = 0;
static DWORD justiceLeagueProjectionGuardReturn = 0;
static DWORD justiceLeagueLuaCompactGuardReturn = 0;
static DWORD justiceLeagueLuaCompactGuardInvalid = 0;
static DWORD justiceLeagueLuaIndexGuardReturn = 0;
static DWORD justiceLeagueLuaIndexGuardInvalid = 0;
static DWORD justiceLeagueLuaIntegerGuardReturn = 0;
static DWORD justiceLeagueLuaIntegerGuardInvalid = 0;
static DWORD justiceLeagueLuaArrayGuardReturn = 0;
static DWORD justiceLeagueLuaArrayGuardInvalid = 0;
static DWORD justiceLeagueLuaConversionSites[4] = {};
static constexpr BYTE justiceLeagueLuaConversionDestinations[4] =
	{ 0x04, 0x10, 0x14, 0x08 };
static PVOID justiceLeagueLuaExceptionHandler = nullptr;
static constexpr DWORD justiceLeagueFloatMultipleFaults = 0xC00002B4;
static constexpr DWORD justiceLeagueFloatMultipleTraps = 0xC00002B5;

static LONG CALLBACK JusticeLeagueLuaFpuExceptionRecovery(
	PEXCEPTION_POINTERS exceptionPointers)
{
	if (!exceptionPointers ||
		!exceptionPointers->ExceptionRecord ||
		!exceptionPointers->ContextRecord)
		return EXCEPTION_CONTINUE_SEARCH;

	const DWORD exceptionCode =
		exceptionPointers->ExceptionRecord->ExceptionCode;
	if (exceptionCode != EXCEPTION_FLT_INVALID_OPERATION &&
		exceptionCode != justiceLeagueFloatMultipleFaults &&
		exceptionCode != justiceLeagueFloatMultipleTraps)
		return EXCEPTION_CONTINUE_SEARCH;

	CONTEXT* context = exceptionPointers->ContextRecord;
	const DWORD exceptionIp = context->Eip;
	for (size_t index = 0;
		index < _countof(justiceLeagueLuaConversionSites);
		++index)
	{
		if (exceptionIp != justiceLeagueLuaConversionSites[index])
			continue;

		const BYTE expectedInstruction[] = {
			0xDB,
			0x5C,
			0x24,
			justiceLeagueLuaConversionDestinations[index]
		};
		if (memcmp(
				reinterpret_cast<const void*>(exceptionIp),
				expectedInstruction,
				sizeof(expectedInstruction)) != 0)
			return EXCEPTION_CONTINUE_SEARCH;

		// Clear the live emulated x87 state as well as the CONTEXT copies below.
		// Without this, Box64 can carry the handled pending exception into Wine's
		// WOW64 transition even though execution resumes past the FISTP site.
		_clearfp();
		unsigned int controlWord = 0;
		_controlfp_s(&controlWord, _MCW_EM, _MCW_EM);

		// Match a masked x87 invalid conversion on Windows: store the integer
		// indefinite value, pop ST(0), clear the saved exception state, and
		// resume at the following FILD instruction. Updating both context copies
		// is required because Wine restores the FXSAVE image on continuation.
		const DWORD integerIndefinite = 0x80000000u;
		memcpy(
			reinterpret_cast<void*>(
				context->Esp + justiceLeagueLuaConversionDestinations[index]),
			&integerIndefinite,
			sizeof(integerIndefinite));

		const DWORD oldTop = (context->FloatSave.StatusWord >> 11) & 7;
		const DWORD newTop = (oldTop + 1) & 7;
		context->FloatSave.ControlWord |= 0x3F;
		context->FloatSave.StatusWord =
			(context->FloatSave.StatusWord & ~(0xFF | (7 << 11))) |
			(newTop << 11);
		context->FloatSave.TagWord |= 3 << (oldTop * 2);

		BYTE* extendedRegisters = context->ExtendedRegisters;
		*reinterpret_cast<WORD*>(extendedRegisters) |= 0x3F;
		WORD* savedStatus = reinterpret_cast<WORD*>(extendedRegisters + 2);
		*savedStatus = static_cast<WORD>(
			(*savedStatus & ~(0xFF | (7 << 11))) | (newTop << 11));
		extendedRegisters[4] &= ~(1 << oldTop);
		DWORD* savedMxCsr =
			reinterpret_cast<DWORD*>(extendedRegisters + 24);
		*savedMxCsr = (*savedMxCsr | 0x1F80) & ~0x3F;

		context->Eip += sizeof(expectedInstruction);
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

static D3DXMATRIX* WINAPI JusticeLeaguePerspectiveFovLHGuard(
	D3DXMATRIX* output,
	FLOAT fieldOfViewY,
	FLOAT aspectRatio,
	FLOAT nearPlane,
	FLOAT farPlane)
{
	constexpr FLOAT minimum = 0.0001f;

	if (!_finite(fieldOfViewY) ||
		fabsf(fieldOfViewY) < minimum ||
		fabsf(sinf(fieldOfViewY * 0.5f)) < minimum)
	{
		fieldOfViewY = D3DX_PI / 4.0f;
	}
	if (!_finite(aspectRatio) || fabsf(aspectRatio) < minimum)
		aspectRatio = 1360.0f / 768.0f;
	if (!_finite(nearPlane) || nearPlane <= minimum)
		nearPlane = 0.1f;
	if (!_finite(farPlane) || farPlane <= nearPlane + minimum)
		farPlane = nearPlane + 1000.0f;

	const FLOAT yScale = 1.0f / tanf(fieldOfViewY * 0.5f);
	const FLOAT xScale = yScale / aspectRatio;
	const FLOAT depthScale = farPlane / (farPlane - nearPlane);

	ZeroMemory(output, sizeof(*output));
	output->_11 = xScale;
	output->_22 = yScale;
	output->_33 = depthScale;
	output->_34 = 1.0f;
	output->_43 = -nearPlane * depthScale;
	return output;
}

// DX9Renderer.dlo asks the engine for the current render dimensions and
// immediately performs an x87 integer divide.  Wine/Winlator can report zero
// during the first Android frame, which raises STATUS_FLOAT_MULTIPLE_TRAPS
// before the renderer has a chance to create its first scene.  Preserve valid
// dimensions and provide the game's native fallback only for that transient
// zero/negative result.
static void __declspec(naked) JusticeLeagueAspectGuard()
{
	__asm
	{
		cmp dword ptr[esp + 18h], 0
		jg widthValid
		mov dword ptr[esp + 18h], 1360
	widthValid:
		cmp dword ptr[esp + 1Ch], 0
		jg heightValid
		mov dword ptr[esp + 1Ch], 768
	heightValid:
		fild dword ptr[esp + 18h]
		mov ecx, dword ptr[esi + 6Ch]
		jmp dword ptr[justiceLeagueAspectGuardReturn]
	}
}

static void __declspec(naked) JusticeLeagueProjectionGuard()
{
	__asm
	{
		cmp dword ptr[esp + 14h], 0
		jg widthValid
		mov dword ptr[esp + 14h], 1360
	widthValid:
		cmp dword ptr[esp + 18h], 0
		jg heightValid
		mov dword ptr[esp + 18h], 768
	heightValid:
		fild dword ptr[esp + 14h]
		fidiv dword ptr[esp + 18h]
		jmp dword ptr[justiceLeagueProjectionGuardReturn]
	}
}

static ULONGLONG __cdecl JusticeLeagueTruncateLuaDoubleBits(
	DWORD lowBits,
	DWORD highBits)
{
	constexpr ULONGLONG mantissaMask = 0x000FFFFFFFFFFFFFULL;
	constexpr ULONGLONG implicitBit = 0x0010000000000000ULL;
	constexpr int invalidResult = static_cast<int>(0x80000000u);
	const auto packResult = [](int value, bool conversionValid)
	{
		return (static_cast<ULONGLONG>(conversionValid ? 1u : 0u) << 32) |
			static_cast<DWORD>(value);
	};

	const DWORD exponent = (highBits >> 20) & 0x7FF;
	if (exponent == 0x7FF)
		return packResult(invalidResult, false);

	const ULONGLONG bits =
		(static_cast<ULONGLONG>(highBits) << 32) | lowBits;
	const ULONGLONG fraction = bits & mantissaMask;
	const int power = static_cast<int>(exponent) - 1023;
	if (power < 0)
	{
		// The Lua call sites accept only exact integers. Positive and negative
		// zero are the only integral IEEE-754 values with an exponent below 0.
		return packResult(0, exponent == 0 && fraction == 0);
	}
	if (power > 31)
		return packResult(invalidResult, false);

	const ULONGLONG significand =
		implicitBit | fraction;
	if (power < 52)
	{
		const ULONGLONG fractionalMask =
			(1ULL << (52 - power)) - 1;
		if ((significand & fractionalMask) != 0)
			return packResult(invalidResult, false);
	}
	const ULONGLONG magnitude = significand >> (52 - power);
	const bool negative = (highBits & 0x80000000u) != 0;

	if (negative)
	{
		if (magnitude > 0x80000000ULL)
			return packResult(invalidResult, false);
		return packResult(
			static_cast<int>(0u - static_cast<DWORD>(magnitude)),
			true);
	}
	if (magnitude > 0x7FFFFFFFULL)
		return packResult(invalidResult, false);
	return packResult(static_cast<int>(magnitude), true);
}

static void __declspec(naked) JusticeLeagueLuaIntegerGuard()
{
	__asm
	{
		// Enter before the original FLD so Box64 never observes a pending x87
		// invalid conversion. Reproduce the exact-integer test from the source
		// double's IEEE-754 bits using integer operations only.
		push eax
		push ecx
		push edx
		push dword ptr[esp + 14h]
		push dword ptr[esp + 14h]
		call JusticeLeagueTruncateLuaDoubleBits
		add esp, 8
		mov dword ptr[esp + 20h], eax
		test edx, edx
		jz invalidConversion
		pop edx
		pop ecx
		pop eax
		jmp dword ptr[justiceLeagueLuaIntegerGuardReturn]
	invalidConversion:
		pop edx
		pop ecx
		pop eax
		jmp dword ptr[justiceLeagueLuaIntegerGuardInvalid]
	}
}

static void __declspec(naked) JusticeLeagueLuaCompactGuard()
{
	__asm
	{
		push eax
		push ecx
		push edx
		push dword ptr[esp + 18h]
		push dword ptr[esp + 18h]
		call JusticeLeagueTruncateLuaDoubleBits
		add esp, 8
		mov dword ptr[esp + 10h], eax
		test edx, edx
		jz invalidConversion
		pop edx
		pop ecx
		pop eax
		jmp dword ptr[justiceLeagueLuaCompactGuardReturn]
	invalidConversion:
		pop edx
		pop ecx
		pop eax
		jmp dword ptr[justiceLeagueLuaCompactGuardInvalid]
	}
}

static void __declspec(naked) JusticeLeagueLuaIndexGuard()
{
	__asm
	{
		push eax
		push ecx
		push edx
		push dword ptr[esp + 28h]
		push dword ptr[esp + 28h]
		call JusticeLeagueTruncateLuaDoubleBits
		add esp, 8
		mov dword ptr[esp + 1Ch], eax
		test edx, edx
		jz invalidConversion
		pop edx
		pop ecx
		pop eax
		jmp dword ptr[justiceLeagueLuaIndexGuardReturn]
	invalidConversion:
		pop edx
		pop ecx
		pop eax
		jmp dword ptr[justiceLeagueLuaIndexGuardInvalid]
	}
}

static void __declspec(naked) JusticeLeagueLuaArrayGuard()
{
	__asm
	{
		// Same integer-only validation used by the Lua array-index validator,
		// with the source qword and destination dword in different stack slots.
		push eax
		push ecx
		push edx
		push dword ptr[esp + 1Ch]
		push dword ptr[esp + 1Ch]
		call JusticeLeagueTruncateLuaDoubleBits
		add esp, 8
		mov dword ptr[esp + 14h], eax
		test edx, edx
		jz invalidConversion
		pop edx
		pop ecx
		pop eax
		jmp dword ptr[justiceLeagueLuaArrayGuardReturn]
	invalidConversion:
		pop edx
		pop ecx
		pop eax
		jmp dword ptr[justiceLeagueLuaArrayGuardInvalid]
	}
}

static DWORD WINAPI InstallJusticeLeagueAspectGuard(LPVOID)
{
	HMODULE renderer = nullptr;
	while (renderer == nullptr)
	{
		renderer = GetModuleHandleA("DX9Renderer.dlo");
		if (renderer == nullptr)
			Sleep(10);
	}

	// Keep the D3DX export replacement opt-in on Android. Patching Wine's
	// relocated D3DX implementation while Box64 is translating its startup path
	// can invalidate a live translation block. The renderer and Lua call-site
	// guards below avoid the observed traps without rewriting Wine code.
	const bool installD3dxGuard =
		getenv("TP_JUSTICE_D3DX_GUARD") != nullptr;
	HMODULE d3dx = GetModuleHandleA("d3dx9_32.dll");
	if (installD3dxGuard && d3dx != nullptr)
	{
		auto perspectiveFovLH = GetProcAddress(
			d3dx,
			"D3DXMatrixPerspectiveFovLH");
		if (perspectiveFovLH != nullptr)
		{
			injector::MakeJMP(
				perspectiveFovLH,
				JusticeLeaguePerspectiveFovLHGuard,
				true);
		}
	}

	auto guardSite = hook::module_pattern(
		renderer,
		"8B 8E 7C 01 00 00 8B 11 8D 44 24 1C 50 8D 44 24 1C 50 "
		"FF 92 90 00 00 00 DB 44 24 18 8B 4E 6C 8B 56 68 DA 74 24 1C")
		.get_first<DWORD>(24);
	justiceLeagueAspectGuardReturn =
		reinterpret_cast<DWORD>(guardSite) + 7;
	injector::MakeJMP(guardSite, JusticeLeagueAspectGuard, true);
	injector::MakeNOP(reinterpret_cast<DWORD>(guardSite) + 5, 2);

	auto projectionGuardSite = hook::module_pattern(
		renderer,
		"DB 44 24 14 DA 74 24 18 DE F9 D9 E8 D9 F3 DC C0")
		.get_first<DWORD>();
	justiceLeagueProjectionGuardReturn =
		reinterpret_cast<DWORD>(projectionGuardSite) + 8;
	injector::MakeJMP(
		projectionGuardSite,
		JusticeLeagueProjectionGuard,
		true);
	injector::MakeNOP(
		reinterpret_cast<DWORD>(projectionGuardSite) + 5,
		3);

	HMODULE lua = GetModuleHandleA("toluaxx.dll");
	if (lua != nullptr)
	{
		auto luaCompactGuardSite = hook::module_pattern(
			lua,
			"DD 44 24 08 DB 5C 24 04 DB 44 24 04 DD 44 24 08 DA E9")
			.get_first<DWORD>();
		justiceLeagueLuaCompactGuardReturn =
			reinterpret_cast<DWORD>(luaCompactGuardSite) + 0x19;
		justiceLeagueLuaCompactGuardInvalid =
			reinterpret_cast<DWORD>(luaCompactGuardSite) + 0x2E;
		injector::MakeJMP(
			luaCompactGuardSite,
			JusticeLeagueLuaCompactGuard,
			true);

		auto luaIndexGuardSite = hook::module_pattern(
			lua,
			"DD 44 24 18 DB 5C 24 10 DB 44 24 10 DD 44 24 18 DA E9")
			.get_first<DWORD>();
		justiceLeagueLuaIndexGuardReturn =
			reinterpret_cast<DWORD>(luaIndexGuardSite) + 0x19;
		justiceLeagueLuaIndexGuardInvalid =
			reinterpret_cast<DWORD>(luaIndexGuardSite) + 0x45;
		injector::MakeJMP(
			luaIndexGuardSite,
			JusticeLeagueLuaIndexGuard,
			true);

		auto luaIntegerGuardSite = hook::module_pattern(
			lua,
			"DD 44 24 04 DB 5C 24 14 DB 44 24 14 DD 07 DA E9")
			.get_first<DWORD>();
		justiceLeagueLuaIntegerGuardReturn =
			reinterpret_cast<DWORD>(luaIntegerGuardSite) + 0x17;
		justiceLeagueLuaIntegerGuardInvalid =
			reinterpret_cast<DWORD>(luaIntegerGuardSite) + 0x28;
		injector::MakeJMP(
			luaIntegerGuardSite,
			JusticeLeagueLuaIntegerGuard,
			true);

		auto luaArrayGuardSite = hook::module_pattern(
			lua,
			"DD 44 24 0C DB 5C 24 08 DB 44 24 08 DD 44 24 0C DA E9")
			.get_first<DWORD>();
		justiceLeagueLuaArrayGuardReturn =
			reinterpret_cast<DWORD>(luaArrayGuardSite) + 0x19;
		justiceLeagueLuaArrayGuardInvalid =
			reinterpret_cast<DWORD>(luaArrayGuardSite) + 0x42;
		injector::MakeJMP(
			luaArrayGuardSite,
			JusticeLeagueLuaArrayGuard,
			true);
	}
	return 0;
}

// controls
extern int* ffbOffset;
extern int* ffbOffset2;
extern int* ffbOffset3;
extern int* ffbOffset4;
// hooks ori
BOOL(__stdcall* original_SetWindowPos8)(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
BOOL(__stdcall* original_CreateWindowExA8)(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
BOOL(__stdcall* original_GetPrivateProfileStringA8)(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault,LPSTR lpReturnedString,DWORD nSize, LPCSTR lpFileName);


DWORD WINAPI InputRT8(LPVOID lpParam)
{
	int deltaTimer = 16;

	while (true)
	{
		// regular buttons are emulated by XINPUTEMU
		// TEST
		if (*ffbOffset & 0x01)
		{
			if (TESTpressed == false)
			{
				keybd_event(0x4F, MapVirtualKey(0x4F, MAPVK_VK_TO_VSC), 0, 0);
				TESTpressed = true;
			}
		}
		else
		{
			if (TESTpressed == true)
			{
				keybd_event(0x4F, MapVirtualKey(0x4F, MAPVK_VK_TO_VSC), KEYEVENTF_KEYUP, 0);
				TESTpressed = false;
			}
		}
		Sleep(deltaTimer);
	}

	return 0;
}

DWORD WINAPI WindowRT8(LPVOID lpParam)
{
	while (true)
	{
		// LEFT-CLICK MOVES WINDOW FROM TOP-LEFT CORNER
		if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
		{
			HWND hWndTMP = GetForegroundWindow();
			if (hWndRT8 == 0)
			{
				hWndRT8 = FindWindowA(NULL, "Justice League");
			}
			if (hWndTMP == hWndRT8)
			{
				POINT point;
				GetCursorPos(&point);
				RECT rect;
				GetWindowRect(hWndRT8, &rect);
				int width = rect.right - rect.left;
				int height = rect.bottom - rect.top;
				LPARAM blah = MAKELPARAM(point.x, point.y);
				int xClick = LOWORD(blah);
				int yClick = HIWORD(blah);
				if ((xClick + (width/2)) > horizontal8)
				{
					xClick = (horizontal8 - width);
				}
				if ((yClick + (height/2)) > vertical8)
				{
					yClick = (vertical8 - height);
				}
				original_SetWindowPos8(hWndRT8, HWND_TOP, xClick, yClick, 1360, 768, SWP_NOSIZE);
				SetForegroundWindow(hWndRT8);

			}
		}
		// RIGHT-CLICK MINIMIZES WINDOW
		if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
		{
			HWND hWndTMP = GetForegroundWindow();
			if (hWndRT8 == 0)
			{
				hWndRT8 = FindWindowA(NULL, "Justice League");
			}
			if (hWndTMP == hWndRT8)
			{
				RECT rect;
				GetWindowRect(hWndRT8, &rect);
				int currentwidth = rect.right - rect.left;
				int currentheight = rect.bottom - rect.top;
				original_SetWindowPos8(hWndRT8, HWND_BOTTOM, 0, 0, 1360, 768, SWP_NOSIZE);
				ShowWindow(hWndRT8, SW_MINIMIZE);
			}
			else ShowWindow(hWndRT8, SW_SHOWDEFAULT);
		}
		// This helper only polls mouse buttons for optional window movement.
		// Yield between polls under Wine so it does not consume an entire CPU
		// core and starve the engine's startup work.
		// Preserve the established native Windows polling behavior.
		if (IsWineCompatEnabled())
			Sleep(16);
	}
}

DWORD WINAPI GetPrivateProfileStringART8(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault, LPSTR lpReturnedString, DWORD nSize, LPCSTR lpFileName)
{
	char horizontalBuffer[16];
	char verticalBuffer[16];
	LPCSTR fs = "true";
	_itoa_s(horizontal8, horizontalBuffer, sizeof(horizontalBuffer), 10);
	_itoa_s(vertical8, verticalBuffer, sizeof(verticalBuffer), 10);
	LPCSTR resX = horizontalBuffer;
	LPCSTR resY = verticalBuffer;
	if (ToBool(config["General"]["Windowed"]))
	{
		fs = "false";
		resX = "1360";
		resY = "768";
	}
	
	if (_stricmp(lpKeyName, "Fullscreen") == 0)
	{	
		WritePrivateProfileStringA(lpAppName, lpKeyName, fs, lpFileName);
	}
	if (_stricmp(lpKeyName, "ScreenResolutionX") == 0)
	{
		WritePrivateProfileStringA(lpAppName, lpKeyName, resX, lpFileName);
	}
	if (_stricmp(lpKeyName, "ScreenResolutionY") == 0)
	{
		WritePrivateProfileStringA(lpAppName, lpKeyName, resY, lpFileName);
	}
	if (_stricmp(lpKeyName, "DisplayWidth") == 0)
	{
		WritePrivateProfileStringA(lpAppName, lpKeyName, resX, lpFileName);
	}
	if (_stricmp(lpKeyName, "DisplayHeight") == 0)
	{
		WritePrivateProfileStringA(lpAppName, lpKeyName, resY, lpFileName);
	}

	return original_GetPrivateProfileStringA8(lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize, lpFileName);
}

DWORD WINAPI CreateWindowExART8(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
	return original_CreateWindowExA8(dwExStyle, lpClassName, "Justice League", 0x96000000, 0, 0, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
}

DWORD WINAPI SetWindowPosRT8(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
	return original_SetWindowPos8(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

static InitFunction JLeagueFunc([]()
{
	GetDesktopResolution(horizontal8, vertical8);

	// DX9Renderer.dlo divides by the render dimensions the moment it is loaded,
	// and any Wine host can still report zero there while Winex11 is mapping
	// the window. The guards below only clamp that transient case.
	if (IsWineCompatEnabled())
	{
		CreateThread(NULL, 0, InstallJusticeLeagueAspectGuard, NULL, 0, NULL);
	}

	CreateThread(NULL, 0, InputRT8, NULL, 0, NULL);

	MH_Initialize();
	MH_CreateHookApi(L"kernel32.dll", "GetPrivateProfileStringA", &GetPrivateProfileStringART8, (void**)& original_GetPrivateProfileStringA8);
	MH_CreateHookApi(L"user32.dll", "CreateWindowExA", &CreateWindowExART8, (void**)& original_CreateWindowExA8);
	MH_CreateHookApi(L"user32.dll", "SetWindowPos", &SetWindowPosRT8, (void**)& original_SetWindowPos8);
	MH_EnableHook(MH_ALL_HOOKS);

	// The optional mouse drag/minimize helper enters User32 from a secondary
	// thread while Wine is still bringing up Winex11. It is not useful with the
	// Android touch overlay and can race Winex11 initialization there, so drop
	// it only on Android; native Windows and desktop Wine keep the helper.
	if (ToBool(config["General"]["Windowed"]) && !IsAndroidWineRuntime())
	{
		CreateThread(NULL, 0, WindowRT8, NULL, 0, NULL);
	}

}, GameID::JLeague);
#endif

#include <StdInc.h>
#include "Utility/InitFunction.h"
#include "Functions/Global.h"
#include "Functions/GlobalRegHooks.h"
#include "Utility\Hooking.Patterns.h"
#include <windows.h>
#include <gameux.h>
#include <cstdarg>
#include <string>
#include <iostream>
#include <shlobj.h>
#include <fstream>
using namespace std;

#include "xinput.h"
#include "Utility/Helper.h"

#pragma comment(lib, "Ws2_32.lib")

static uintptr_t imageBase;
int horizontal6 = 0;
int vertical6 = 0;
HWND hWndRT6 = 0;

extern int* ffbOffset;
extern int* ffbOffset2;
extern int* ffbOffset3;

// hooks ori
BOOL(__stdcall *original_SetWindowPos6)(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
BOOL(__stdcall *original_CreateWindowExA6)(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
BOOL(__stdcall *original_DefWindowProcA6)(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
BOOL(__stdcall *original_SetCursorPosRT6)(int X, int Y);
BOOL(__stdcall *original_SetWindowTextWRT6)(HWND hWnd, LPCWSTR lpString);
HRESULT(WINAPI *original_CoCreateInstanceGHA)(
	REFCLSID rclsid,
	LPUNKNOWN pUnkOuter,
	DWORD dwClsContext,
	REFIID riid,
	LPVOID* ppv);
HRESULT(STDMETHODCALLTYPE *original_GameExplorerVerifyAccessGHA)(
	IGameExplorer* self,
	BSTR gdfBinaryPath,
	BOOL* hasAccess);
static HRESULT WINAPI CoCreateInstanceGHA(
	REFCLSID rclsid,
	LPUNKNOWN pUnkOuter,
	DWORD dwClsContext,
	REFIID riid,
	LPVOID* ppv);

static bool IsGHARegistryDiagnosticsEnabled()
{
	const char* variables[] = {
		"TP_ANDROID_DEBUG_LOGGING",
		"TP_GHA_LOCAL_DIAGNOSTICS"
	};
	for (const char* variable : variables)
	{
		char value[8] = {};
		const DWORD length = GetEnvironmentVariableA(
			variable,
			value,
			_countof(value));
		if (length > 0 &&
			length < _countof(value) &&
			value[0] == '1')
		{
			return true;
		}
	}
	return false;
}

static void LogGHARegistryDiagnostic(const char* format, ...)
{
	if (!IsGHARegistryDiagnosticsEnabled())
		return;

	char line[1024] = {};
	va_list arguments;
	va_start(arguments, format);
	const int length = vsnprintf_s(
		line,
		sizeof(line),
		_TRUNCATE,
		format,
		arguments);
	va_end(arguments);
	if (length <= 0)
		return;

	HANDLE file = CreateFileA(
		"OpenParrotGHARegistry.log",
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
	WriteFile(file, "\r\n", 2, &written, nullptr);
	CloseHandle(file);
}

static std::string GHARegistryText(LPCWSTR value)
{
	if (value == nullptr)
		return "<null>";
	const int required = WideCharToMultiByte(
		CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
	if (required <= 1)
		return std::string();
	std::string result(static_cast<size_t>(required), '\0');
	WideCharToMultiByte(
		CP_UTF8,
		0,
		value,
		-1,
		result.data(),
		required,
		nullptr,
		nullptr);
	result.pop_back();
	return result;
}

static LSTATUS __stdcall RegOpenKeyExWGHA(
	HKEY hKey,
	LPCWSTR lpSubKey,
	DWORD ulOptions,
	REGSAM samDesired,
	PHKEY phkResult)
{
	const LSTATUS result = RegOpenKeyExWGlobalWrap(
		hKey, lpSubKey, ulOptions, samDesired, phkResult);
	LogGHARegistryDiagnostic(
		"RegOpenKeyExW root=%p key=%s result=%ld handle=%p",
		hKey,
		GHARegistryText(lpSubKey).c_str(),
		result,
		phkResult != nullptr ? *phkResult : nullptr);
	return result;
}

static LSTATUS __stdcall RegCreateKeyExWGHA(
	HKEY hKey,
	LPCWSTR lpSubKey,
	DWORD Reserved,
	LPWSTR lpClass,
	DWORD dwOptions,
	REGSAM samDesired,
	CONST LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	PHKEY phkResult,
	LPDWORD lpdwDisposition)
{
	const LSTATUS result = RegCreateKeyExWGlobalWrap(
		hKey,
		lpSubKey,
		Reserved,
		lpClass,
		dwOptions,
		samDesired,
		lpSecurityAttributes,
		phkResult,
		lpdwDisposition);
	LogGHARegistryDiagnostic(
		"RegCreateKeyExW root=%p key=%s result=%ld handle=%p disposition=%lu",
		hKey,
		GHARegistryText(lpSubKey).c_str(),
		result,
		phkResult != nullptr ? *phkResult : nullptr,
		lpdwDisposition != nullptr ? *lpdwDisposition : 0);
	return result;
}

static LSTATUS __stdcall RegQueryValueExWGHA(
	HKEY hKey,
	LPCWSTR lpValueName,
	LPDWORD lpReserved,
	LPDWORD lpType,
	__out_data_source(REGISTRY) LPBYTE lpData,
	LPDWORD lpcbData)
{
	const LSTATUS result = RegQueryValueExWGlobalWrap(
		hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
	LogGHARegistryDiagnostic(
		"RegQueryValueExW handle=%p value=%s result=%ld type=%lu size=%lu",
		hKey,
		GHARegistryText(lpValueName).c_str(),
		result,
		lpType != nullptr ? *lpType : 0,
		lpcbData != nullptr ? *lpcbData : 0);
	if (result == ERROR_SUCCESS &&
		lpType != nullptr &&
		(*lpType == REG_SZ || *lpType == REG_EXPAND_SZ) &&
		lpData != nullptr &&
		lpcbData != nullptr &&
		*lpcbData >= sizeof(wchar_t))
	{
		LogGHARegistryDiagnostic(
			"RegQueryValueExW value=%s text=%s",
			GHARegistryText(lpValueName).c_str(),
			GHARegistryText(
				reinterpret_cast<LPCWSTR>(lpData)).c_str());
	}
	return result;
}

template<typename T>
static bool HookImportedFunctionGHA(
	HMODULE module,
	const char* importedModule,
	const char* importedFunction,
	T replacement,
	T* original = nullptr)
{
	if (module == nullptr)
		return false;

	auto* base = reinterpret_cast<unsigned char*>(module);
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;

	const DWORD importRva =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	if (importRva == 0)
		return false;

	auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importRva);
	for (; descriptor->Name != 0; ++descriptor)
	{
		const char* moduleName =
			reinterpret_cast<const char*>(base + descriptor->Name);
		if (_stricmp(moduleName, importedModule) != 0)
			continue;
		if (descriptor->OriginalFirstThunk == 0)
			return false;

		auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
			base + descriptor->OriginalFirstThunk);
		auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
			base + descriptor->FirstThunk);
		for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++addressThunk)
		{
			if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal))
				continue;
			auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
				base + nameThunk->u1.AddressOfData);
			if (_stricmp(
					reinterpret_cast<const char*>(import->Name),
					importedFunction) != 0)
				continue;

			if (original != nullptr)
				*original = reinterpret_cast<T>(addressThunk->u1.Function);
			injector::WriteMemory(
				&addressThunk->u1.Function,
				reinterpret_cast<uintptr_t>(replacement),
				true);
			return true;
		}
		return false;
	}
	return false;
}

static bool InstallGHARegistryIatHooks()
{
	HMODULE awlModule = GetModuleHandleA("AWL.dll");
	if (awlModule == nullptr)
		return false;

	const bool openHooked = HookImportedFunctionGHA(
		awlModule,
		"advapi32.dll",
		"RegOpenKeyExW",
		&RegOpenKeyExWGHA);
	const bool createHooked = HookImportedFunctionGHA(
		awlModule,
		"advapi32.dll",
		"RegCreateKeyExW",
		&RegCreateKeyExWGHA);
	const bool queryHooked = HookImportedFunctionGHA(
		awlModule,
		"advapi32.dll",
		"RegQueryValueExW",
		&RegQueryValueExWGHA);
	const bool coCreateHooked = HookImportedFunctionGHA(
		awlModule,
		"ole32.dll",
		"CoCreateInstance",
		&CoCreateInstanceGHA,
		&original_CoCreateInstanceGHA);
	LogGHARegistryDiagnostic(
		"AWL=%p hooks open=%d create=%d query=%d coCreate=%d",
		awlModule,
		openHooked ? 1 : 0,
		createHooked ? 1 : 0,
		queryHooked ? 1 : 0,
		coCreateHooked ? 1 : 0);
	return openHooked && createHooked && queryHooked && coCreateHooked;
}

static DWORD WINAPI InstallGHADeferredIatHooks(LPVOID)
{
	// A CREATE_SUSPENDED Wine process can receive OpenParrot before the
	// executable's static dependencies have been mapped. Patch AWL as soon as
	// the loader exposes it, before the game's first registry validation.
	for (int attempt = 0; attempt < 10000; ++attempt)
	{
		if (InstallGHARegistryIatHooks())
			return 0;
		Sleep(1);
	}
	LogGHARegistryDiagnostic("AWL registry imports were not available after 10s");
	return ERROR_MOD_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE GameExplorerVerifyAccessGHA(
	IGameExplorer* self,
	BSTR gdfBinaryPath,
	BOOL* hasAccess)
{
	if (hasAccess != nullptr)
		*hasAccess = TRUE;
	return S_OK;
}

static HRESULT WINAPI CoCreateInstanceGHA(
	REFCLSID rclsid,
	LPUNKNOWN pUnkOuter,
	DWORD dwClsContext,
	REFIID riid,
	LPVOID* ppv)
{
	const HRESULT result = original_CoCreateInstanceGHA(
		rclsid,
		pUnkOuter,
		dwClsContext,
		riid,
		ppv);

	if (SUCCEEDED(result) &&
		ppv != nullptr &&
		*ppv != nullptr &&
		IsEqualIID(riid, __uuidof(IGameExplorer)))
	{
		auto* gameExplorer = static_cast<IGameExplorer*>(*ppv);
		void* verifyAccess = (*reinterpret_cast<void***>(gameExplorer))[6];
		const MH_STATUS hookResult = MH_CreateHook(
			verifyAccess,
			&GameExplorerVerifyAccessGHA,
			reinterpret_cast<void**>(&original_GameExplorerVerifyAccessGHA));
		if (hookResult == MH_OK || hookResult == MH_ERROR_ALREADY_CREATED)
			MH_EnableHook(verifyAccess);
	}

	return result;
}

DWORD WINAPI WindowRT6(LPVOID lpParam)
{
	while (true)
	{
		// RIGHT-CLICK MINIMIZES WINDOW
		if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
		{
			HWND hWndTMP = GetForegroundWindow();
			if (hWndRT6 == 0)
			{
				hWndRT6 = FindWindowA(NULL, "Guitar Hero Arcade");
			}
			if (hWndTMP == hWndRT6)
			{
				RECT rect;
				GetWindowRect(hWndRT6, &rect);
				int currentwidth = rect.right - rect.left;
				int currentheight = rect.bottom - rect.top;
				original_SetWindowPos6(hWndRT6, HWND_BOTTOM, 0, 0, 1360, 768, SWP_NOSIZE);
				ShowWindow(hWndRT6, SW_MINIMIZE);
			}
		}
	}
}

static bool init;
static bool LeftStart;
static bool LeftStrumUp;
static bool LeftStrumDown;
static bool LeftGreen;
static bool LeftRed;
static bool LeftYellow;
static bool LeftBlue;
static bool LeftOrange;
static bool RightStart;
static bool RightStrumUp;
static bool RightStrumDown;
static bool RightGreen;
static bool RightRed;
static bool RightYellow;
static bool RightBlue;
static bool RightOrange;

void GHAInputs(Helpers* helpers)
{
	DWORD Buttons = helpers->ReadInt32(0x746AB0, true);
	BYTE Active = *(BYTE*)(imageBase + 0x857AE0);

	if (!init)
	{
		BYTE Modify = *(BYTE*)(imageBase + 0x13B3DC4);

		if (Modify)
		{
			init = true;
			*(BYTE*)(imageBase + 0x857AE0) = 0x00;
		}
	}
	else
	{
		if (*ffbOffset & 0x01) // Left Start
		{
			if (!LeftStart)
			{
				LeftStart = true;

				if (!Active)
					*(BYTE*)(Buttons + 0x85) = 0x08;
			}
			else if (!Active)
				*(BYTE*)(Buttons + 0x85) = 0x00;
		}
		else
		{
			if (LeftStart)
			{
				LeftStart = false;

				if (!Active)
					*(BYTE*)(Buttons + 0x85) = 0x00;
			}
		}

		if (*ffbOffset & 0x02) // Left Strum Up
		{
			if (!LeftStrumUp)
			{
				LeftStrumUp = true;
				*(BYTE*)(Buttons + 0x75) += 0x10;

				if (Active)
					*(BYTE*)(Buttons + 0x84) = 0x01;
			}
			else if (Active)
				*(BYTE*)(Buttons + 0x84) = 0x00;
		}
		else
		{
			if (LeftStrumUp)
			{
				LeftStrumUp = false;
				*(BYTE*)(Buttons + 0x75) -= 0x10;

				if (Active)
					*(BYTE*)(Buttons + 0x84) = 0x00;
			}
		}

		if (*ffbOffset & 0x04) // Left Strum Down
		{
			if (!LeftStrumDown)
			{
				LeftStrumDown = true;
				*(BYTE*)(Buttons + 0x75) += 0x40;

				if (Active)
					*(BYTE*)(Buttons + 0x84) = 0x01;
			}
			else if (Active)
				*(BYTE*)(Buttons + 0x84) = 0x00;
		}
		else
		{
			if (LeftStrumDown)
			{
				LeftStrumDown = false;
				*(BYTE*)(Buttons + 0x75) -= 0x40;

				if (Active)
					*(BYTE*)(Buttons + 0x84) = 0x00;
			}
		}

		if (*ffbOffset & 0x08) // Left Green Fret
		{
			if (!LeftGreen)
			{
				LeftGreen = true;
				*(BYTE*)(Buttons + 0x52) += 0xFF;

				if (!Active)
					*(BYTE*)(Buttons + 0x84) = 0x40;
			}
			else if (!Active)
				*(BYTE*)(Buttons + 0x84) = 0x00;
		}
		else
		{
			if (LeftGreen)
			{
				LeftGreen = false;
				*(BYTE*)(Buttons + 0x52) -= 0xFF;

				if (!Active)
					*(BYTE*)(Buttons + 0x84) = 0x00;
			}
		}

		if (*ffbOffset & 0x10) // Left Red Fret
		{
			if (!LeftRed)
			{
				LeftRed = true;
				*(BYTE*)(Buttons + 0x50) += 0xFF;

				if (!Active)
					*(BYTE*)(Buttons + 0x85) = 0x01;
			}
			else if (!Active)
				*(BYTE*)(Buttons + 0x85) = 0x00;
		}
		else
		{
			if (LeftRed)
			{
				LeftRed = false;
				*(BYTE*)(Buttons + 0x50) -= 0xFF;

				if (!Active)
					*(BYTE*)(Buttons + 0x85) = 0x00;
			}
		}

		if (*ffbOffset & 0x20) // Left Yellow Fret
		{
			if (!LeftYellow)
			{
				LeftYellow = true;
				*(BYTE*)(Buttons + 0x51) += 0xFF;
			}
		}
		else
		{
			if (LeftYellow)
			{
				LeftYellow = false;
				*(BYTE*)(Buttons + 0x51) -= 0xFF;
			}
		}

		if (*ffbOffset & 0x40) // Left Blue Fret
		{
			if (!LeftBlue)
			{
				LeftBlue = true;
				*(BYTE*)(Buttons + 0x53) += 0xFF;
			}
		}
		else
		{
			if (LeftBlue)
			{
				LeftBlue = false;
				*(BYTE*)(Buttons + 0x53) -= 0xFF;
			}
		}

		if (*ffbOffset & 0x80) // Left Orange Fret
		{
			if (!LeftOrange)
			{
				LeftOrange = true;
				*(BYTE*)(Buttons + 0x4E) += 0xFF;
			}
		}
		else
		{
			if (LeftOrange)
			{
				LeftOrange = false;
				*(BYTE*)(Buttons + 0x4E) -= 0xFF;
			}
		}

		if (*ffbOffset & 0x100) // Right Start
		{
			if (!RightStart)
			{
				RightStart = true;

				if (!Active)
					*(BYTE*)(Buttons + 0x121) = 0x08;
			}
			else if (!Active)
				*(BYTE*)(Buttons + 0x121) = 0x00;
		}
		else
		{
			if (RightStart)
			{
				RightStart = false;

				if (!Active)
					*(BYTE*)(Buttons + 0x121) = 0x00;
			}
		}

		if (*ffbOffset & 0x200) // Right Strum Up
		{
			if (!RightStrumUp)
			{
				RightStrumUp = true;
				*(BYTE*)(Buttons + 0x111) += 0x10;

				if (Active)
					*(BYTE*)(Buttons + 0x120) = 0x01;
			}
			else if (Active)
				*(BYTE*)(Buttons + 0x120) = 0x00;
		}
		else
		{
			if (RightStrumUp)
			{
				RightStrumUp = false;
				*(BYTE*)(Buttons + 0x111) -= 0x10;

				if (Active)
					*(BYTE*)(Buttons + 0x120) = 0x00;
			}
		}

		if (*ffbOffset & 0x400) // Right Strum Down
		{
			if (!RightStrumDown)
			{
				RightStrumDown = true;
				*(BYTE*)(Buttons + 0x111) += 0x40;

				if (Active)
					*(BYTE*)(Buttons + 0x120) = 0x01;
			}
			else if (Active)
				*(BYTE*)(Buttons + 0x120) = 0x00;
		}
		else
		{
			if (RightStrumDown)
			{
				RightStrumDown = false;
				*(BYTE*)(Buttons + 0x111) -= 0x40;

				if (Active)
					*(BYTE*)(Buttons + 0x120) = 0x00;
			}
		}

		if (*ffbOffset & 0x800) // Right Green Fret
		{
			if (!RightGreen)
			{
				RightGreen = true;
				*(BYTE*)(Buttons + 0xEE) += 0xFF;

				if (!Active)
					*(BYTE*)(Buttons + 0x120) = 0x40;
			}
			else if (!Active)
				*(BYTE*)(Buttons + 0x120) = 0x00;
		}
		else
		{
			if (RightGreen)
			{
				RightGreen = false;
				*(BYTE*)(Buttons + 0xEE) -= 0xFF;

				if (!Active)
					*(BYTE*)(Buttons + 0x120) = 0x00;
			}
		}

		if (*ffbOffset & 0x1000) // Right Red Fret
		{
			if (!RightRed)
			{
				RightRed = true;
				*(BYTE*)(Buttons + 0xEC) += 0xFF;

				if (!Active)
					*(BYTE*)(Buttons + 0x121) = 0x01;
			}
			else if (!Active)
				*(BYTE*)(Buttons + 0x121) = 0x00;
		}
		else
		{
			if (RightRed)
			{
				RightRed = false;
				*(BYTE*)(Buttons + 0xEC) -= 0xFF;

				if (!Active)
					*(BYTE*)(Buttons + 0x121) = 0x00;
			}
		}

		if (*ffbOffset & 0x2000) // Right Yellow Fret
		{
			if (!RightYellow)
			{
				RightYellow = true;
				*(BYTE*)(Buttons + 0xED) += 0xFF;
			}
		}
		else
		{
			if (RightYellow)
			{
				RightYellow = false;
				*(BYTE*)(Buttons + 0xED) -= 0xFF;
			}
		}

		if (*ffbOffset & 0x4000) // Right Blue Fret
		{
			if (!RightBlue)
			{
				RightBlue = true;
				*(BYTE*)(Buttons + 0xEF) += 0xFF;
			}
		}
		else
		{
			if (RightBlue)
			{
				RightBlue = false;
				*(BYTE*)(Buttons + 0xEF) -= 0xFF;
			}
		}

		if (*ffbOffset & 0x8000) // Right Orange Fret
		{
			if (!RightOrange)
			{
				RightOrange = true;
				*(BYTE*)(Buttons + 0xEA) += 0xFF;
			}
		}
		else
		{
			if (RightOrange)
			{
				RightOrange = false;
				*(BYTE*)(Buttons + 0xEA) -= 0xFF;
			}
		}

		*(BYTE*)(Buttons + 0x66) = *ffbOffset2; // Left Guitar Tilt
		*(BYTE*)(Buttons + 0x102) = *ffbOffset3; // Right Guitar Tilt
	}
}

static DWORD WINAPI RunningLoop(LPVOID lpParam)
{
	Helpers helpers;
	while (true)
	{
		GHAInputs(&helpers);
		Sleep(16);
	}
}

DWORD WINAPI DefWindowProcART6(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	static int xClick;
	static int yClick;

	switch (message)
	{
	case WM_LBUTTONDOWN:
		SetCapture(hWnd);
		xClick = LOWORD(lParam);
		yClick = HIWORD(lParam);
		break;

	case WM_LBUTTONUP:
		ReleaseCapture();
		break;

	case WM_MOUSEMOVE:
	{
		if (GetCapture() == hWnd)
		{
			RECT rcWindow;
			GetWindowRect(hWnd, &rcWindow);
			int xMouse = LOWORD(lParam);
			int yMouse = HIWORD(lParam);
			int xWindow = rcWindow.left + xMouse - xClick;
			int yWindow = rcWindow.top + yMouse - yClick;
			if (xWindow >= (horizontal6 - 100))
				xWindow = 0;
			if (yWindow >= (vertical6 - 100))
				yWindow = 0;
			original_SetWindowPos6(hWnd, NULL, xWindow, yWindow, 1360, 768, SWP_NOSIZE | SWP_NOZORDER);
		}
		break;
	}

	}
	return original_DefWindowProcA6(hWnd, message, wParam, lParam);
}

DWORD WINAPI CreateWindowExART6(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
	return original_CreateWindowExA6(dwExStyle, lpClassName, "Guitar Hero Arcade", 0x94000000, X, Y, 1360, 768, hWndParent, hMenu, hInstance, lpParam);
}

DWORD WINAPI SetCursorPosRT6(int X, int Y)
{
	return 1;
}

DWORD WINAPI SetWindowPosRT6(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
	return 1;
}

DWORD WINAPI SetWindowTextWRT6(HWND hWnd, LPCWSTR lpString)
{
	return original_SetWindowTextWRT6(hWnd, L"Guitar Hero Arcade");
}

static InitFunction GHAFunc([]()
{
	char diagnosticWorkingDirectory[MAX_PATH + 1] = {};
	GetCurrentDirectoryA(
		static_cast<DWORD>(std::size(diagnosticWorkingDirectory)),
		diagnosticWorkingDirectory);
	LogGHARegistryDiagnostic(
		"GHAFunc entered currentDirectory=%s image=%p",
		diagnosticWorkingDirectory,
		GetModuleHandleA(nullptr));

	imageBase = (uintptr_t)GetModuleHandleA(0);

	init_GlobalRegHooks();
	// AWL.dll owns the Aspyr registry checks. Wine's experimental WOW64 path
	// can bypass the advapi32 export hooks, just as it can bypass ole32 below,
	// so bind AWL's three relevant imports directly to the existing GHA-aware
	// wrappers. This remains title-local and leaves every other game's IAT
	// untouched.
	if (!InstallGHARegistryIatHooks())
		CreateThread(nullptr, 0, InstallGHADeferredIatHooks, nullptr, 0, nullptr);
	// Wine's experimental WOW64 path can bypass MinHook's ole32 export
	// trampoline while still calling the executable's imported function.
	// Bind the game's IAT directly so the access check is intercepted before
	// the original call can terminate GHA.
	if (original_CoCreateInstanceGHA == nullptr)
	{
		original_CoCreateInstanceGHA =
			iatHook("ole32.dll", CoCreateInstanceGHA, "CoCreateInstance");
	}
	if (original_CoCreateInstanceGHA == nullptr)
	{
		MH_CreateHookApi(
			L"ole32.dll",
			"CoCreateInstance",
			&CoCreateInstanceGHA,
			reinterpret_cast<void**>(&original_CoCreateInstanceGHA));
	}
	MH_EnableHook(MH_ALL_HOOKS);
	GetDesktopResolution(horizontal6, vertical6);

	// Disable Xinput Inputs
	injector::MakeNOP(imageBase + 0x132168, 4);
	injector::MakeNOP(imageBase + 0x13215C, 4);
	injector::MakeNOP(imageBase + 0x1089F0, 3);
	injector::MakeNOP(imageBase + 0x108A01, 3);
	injector::MakeNOP(imageBase + 0x108A07, 3);
	injector::MakeNOP(imageBase + 0x108AEE, 3);

	// CONFIG FILE HANDLING
	char working_directory[MAX_PATH + 1];
	GetCurrentDirectoryA(sizeof(working_directory), working_directory);
	std::string s7 = working_directory;
	std::string s8 = s7 + "\\AspyrConfig.xml";

	ofstream file;
	file.open(s8.c_str());

	string strXML = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<r>\n	<s id=\"Video.Width\">";

	std::string h = std::to_string(horizontal6);
	std::string v = std::to_string(vertical6);
	
	if (ToBool(config["General"]["Windowed"]))
	{
	strXML += "1360";
	}
	else if (ToBool(config["General"]["HDPatch"]))
	{
		strXML += h;
	}
	else 
	{
		strXML += "1360";
	}

	strXML += "</s>\n	<s id=\"Video.Height\">";

	if (ToBool(config["General"]["Windowed"]))
	{
		strXML += "768";
	}
	else if (ToBool(config["General"]["HDPatch"]))
	{
		strXML += v;
	}
	else 
	{
		strXML += "768";
	}
	
	strXML += "</s> \n	<s id=\"Options.GraphicsQuality\">0</s>\n	<s id=\"Options.Crowd\">1</s>\n	<s id=\"Options.Physics\">1</s>\n	<s id=\"Options.Flares\">1</s>\n	<s id=\"Options.FrontRowCamera\">0</s>\n	<s id=\"6f1d2b61d5a011cfbfc7444553540000\">328 221 340 343 267 264 999 219 235 331 304 999 310</s>\n</r>\n";
	file << strXML;
	file.close();

	char user[MAX_PATH];
	SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, user);
	std::string s11 = user;
	std::string s12 = s11 + "\\Aspyr\\Guitar Hero III\\AspyrConfig.xml";
	SetFileAttributesA(s12.c_str(), FILE_ATTRIBUTE_NORMAL);
	Sleep(100);
	CopyFileA(s8.c_str(), s12.c_str(), FALSE);
	Sleep(100);
	SetFileAttributesA(s12.c_str(), FILE_ATTRIBUTE_READONLY);
	Sleep(100);

	// UNLOCK MAIN MENU
	std::string s21 = s7 + "\\DATA\\PAK\\qb.pab.xen";
	std::string s22 = s7 + "\\DATA\\PAK\\qb.pak.xen";
	std::string s23 = s7 + "\\DATA\\PAK\\qb.pab.ATTRACTv4.xen";
	std::string s24 = s7 + "\\DATA\\PAK\\qb.pak.ATTRACTv4.xen";
	std::string s25 = s7 + "\\DATA\\PAK\\qb.pab.MENUv4.xen";
	std::string s26 = s7 + "\\DATA\\PAK\\qb.pak.MENUv4.xen";

	if (ToBool(config["General"]["UnlockMainMenu"]))
	{
		CopyFileA(s25.c_str(), s21.c_str(), FALSE);
		CopyFileA(s26.c_str(), s22.c_str(), FALSE);
		Sleep(100);
	}
	else
	{
		CopyFileA(s23.c_str(), s21.c_str(), FALSE);
		CopyFileA(s24.c_str(), s22.c_str(), FALSE);
		Sleep(100);
	}

	MH_Initialize();
	MH_CreateHookApi(L"user32.dll", "SetWindowPos", &SetWindowPosRT6, (void**)&original_SetWindowPos6);
	MH_CreateHookApi(L"user32.dll", "SetWindowTextW", &SetWindowTextWRT6, (void**)&original_SetWindowTextWRT6);
	MH_EnableHook(MH_ALL_HOOKS);

	if (ToBool(config["General"]["Windowed"]))
	{
		CreateThread(NULL, 0, WindowRT6, NULL, 0, NULL);

		MH_Initialize();
		MH_CreateHookApi(L"user32.dll", "CreateWindowExA", &CreateWindowExART6, (void**)&original_CreateWindowExA6);
		MH_CreateHookApi(L"user32.dll", "DefWindowProcA", &DefWindowProcART6, (void**)&original_DefWindowProcA6);
		MH_CreateHookApi(L"user32.dll", "SetCursorPos", &SetCursorPosRT6, NULL);
		MH_EnableHook(MH_ALL_HOOKS);
	}

	CreateThread(NULL, 0, RunningLoop, NULL, 0, NULL);

}, GameID::GHA);

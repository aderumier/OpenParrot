#include <StdInc.h>
#include "Utility/InitFunction.h"
#include "Functions/Global.h"
#include <deque>
#include <d3d9.h>
// BASED ON xb_monitor by zxmarcos https://github.com/zxmarcos/xb_monitor
#if _M_IX86

#define SRAM_SIZE	0xffff

using namespace std;

static int isAddressedExBoard = 0;
int is_addressedExBoard() {
	return isAddressedExBoard;
}
void reset_addressedExBoard()
{
	isAddressedExBoard = 0;
}

static vector<char> r(1024);

unsigned char SRAM[SRAM_SIZE];

static bool ShouldForceSoftwareDirectSoundExBoard()
{
	char value[8] = {};
	const DWORD length = GetEnvironmentVariableA(
		"TP_EXBOARD_SOFTWARE_DSOUND", value, static_cast<DWORD>(sizeof(value)));
	return length == 1 && value[0] == '1';
}

static bool IsDaemonBrideExBoard()
{
	char modulePath[MAX_PATH] = {};
	if (GetModuleFileNameA(nullptr, modulePath, _countof(modulePath)) == 0)
		return false;

	const char* fileName = strrchr(modulePath, '\\');
	fileName = fileName == nullptr ? modulePath : fileName + 1;
	return _stricmp(fileName, "DB1.exe") == 0;
}

static const char* ResolveDaemonBrideFlashRomPath(LPCSTR fileName)
{
	if (fileName == nullptr)
		return nullptr;

	constexpr size_t prefixLength = 9;
	if (_strnicmp(fileName, "FlashRom\\", prefixLength) != 0 &&
		_strnicmp(fileName, "FlashRom/", prefixLength) != 0)
		return nullptr;
	if (!IsDaemonBrideExBoard())
		return nullptr;

	const char* relativePath = fileName + prefixLength;
	return GetFileAttributesA(fileName) == INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(relativePath) != INVALID_FILE_ATTRIBUTES
		? relativePath
		: nullptr;
}

static bool IsExBoardRunningUnderWine()
{
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	return ntdll != nullptr &&
		GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

// DB1 renders into a 16-bit R5G6B5 D3D9 back buffer.  Under Wine/DXVK the
// unwrapped Present path can keep the X11 surface black even though the game
// continues presenting at 60 Hz.  A no-state-change Present trampoline makes
// the surface visible.  Keep this DB1 + Wine scoped so native Windows retains
// its original D3D9 path.
static decltype(&Direct3DCreate9) DaemonBrideDirect3DCreate9Original = nullptr;
using DaemonBrideCreateDevice = HRESULT(WINAPI*)(
	IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
	D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using DaemonBridePresent = HRESULT(WINAPI*)(
	IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

static DaemonBrideCreateDevice DaemonBrideCreateDeviceOriginal = nullptr;
static DaemonBridePresent DaemonBridePresentOriginal = nullptr;

static void InstallDaemonBrideD3D9VtableHook(
	void** vtable,
	size_t index,
	void* hook,
	void** original)
{
	if (vtable == nullptr || hook == nullptr || original == nullptr)
		return;
	if (*original == nullptr)
		*original = vtable[index];
	if (vtable[index] == hook)
		return;

	DWORD oldProtection = 0;
	if (!VirtualProtect(
			&vtable[index],
			sizeof(vtable[index]),
			PAGE_EXECUTE_READWRITE,
			&oldProtection))
		return;
	vtable[index] = hook;
	DWORD ignored = 0;
	VirtualProtect(
		&vtable[index],
		sizeof(vtable[index]),
		oldProtection,
		&ignored);
	FlushInstructionCache(
		GetCurrentProcess(),
		&vtable[index],
		sizeof(vtable[index]));
}

static HRESULT WINAPI DaemonBridePresentHook(
	IDirect3DDevice9* device,
	const RECT* sourceRect,
	const RECT* destinationRect,
	HWND destinationWindowOverride,
	const RGNDATA* dirtyRegion)
{
	return DaemonBridePresentOriginal(
		device,
		sourceRect,
		destinationRect,
		destinationWindowOverride,
		dirtyRegion);
}

static HRESULT WINAPI DaemonBrideCreateDeviceHook(
	IDirect3D9* direct3D,
	UINT adapter,
	D3DDEVTYPE deviceType,
	HWND focusWindow,
	DWORD behaviorFlags,
	D3DPRESENT_PARAMETERS* parameters,
	IDirect3DDevice9** returnedDevice)
{
	const HRESULT result = DaemonBrideCreateDeviceOriginal(
		direct3D,
		adapter,
		deviceType,
		focusWindow,
		behaviorFlags,
		parameters,
		returnedDevice);
	if (SUCCEEDED(result) && returnedDevice != nullptr && *returnedDevice != nullptr)
	{
		void** vtable = *reinterpret_cast<void***>(*returnedDevice);
		InstallDaemonBrideD3D9VtableHook(
			vtable,
			17,
			reinterpret_cast<void*>(DaemonBridePresentHook),
			reinterpret_cast<void**>(&DaemonBridePresentOriginal));
	}
	return result;
}

static IDirect3D9* WINAPI DaemonBrideDirect3DCreate9Hook(UINT sdkVersion)
{
	IDirect3D9* result = DaemonBrideDirect3DCreate9Original(sdkVersion);
	if (result != nullptr)
	{
		void** vtable = *reinterpret_cast<void***>(result);
		InstallDaemonBrideD3D9VtableHook(
			vtable,
			16,
			reinterpret_cast<void*>(DaemonBrideCreateDeviceHook),
			reinterpret_cast<void**>(&DaemonBrideCreateDeviceOriginal));
	}
	return result;
}

void SRAM_save()
{
	FILE *fp = NULL;
	fp = fopen("sram.bin", "wb");
	if (!fp) {
		return;
	}
	fwrite(SRAM, 1, SRAM_SIZE, fp);
	fclose(fp);
}

void SRAM_load()
{
	FILE *fp = NULL;
	fp = fopen("sram.bin", "rb");
	memset(SRAM, 0, SRAM_SIZE);
	if (!fp) {
		return;
	}
	fread(SRAM, 1, SRAM_SIZE, fp);
	fclose(fp);
}

DWORD process_streamExBoard(UINT8 *stream, DWORD srcsize, BYTE *dst, DWORD dstsize)
{

	r.clear();

	switch (stream[1]) {
	case 0xfa:
	{
		unsigned int addr = 0;
		unsigned int size = 0;
		r.push_back(0x76);
		r.push_back(0xfa);
		r.push_back(0x05);
		r.push_back(0x70);
		r.push_back(0x42);
		//A5 FA 17 00 58 10 02 03 04 05 06 00 01 02 03 04 05 06 00 00 00 00 5A 
		addr = (stream[3] << 8) | (stream[4]);
		size = stream[5];

		if (size >= (srcsize - 1))
			size = srcsize - 1;
		if ((addr + size) >= 0xffff)
			size = 0xffff - addr;

		//logmsg("SRAM WRITE %d : %x\n", size, addr);
		memcpy(&SRAM[addr], &stream[6], size);
		SRAM_save();
		break;
	}

	case 0xfb:
	{
		//A5 FB 07 00 00 EF 5A 
		r.push_back(0x76);
		r.push_back(0xfb);

		unsigned int addr = 0;
		unsigned int size = stream[5];
		unsigned pos = r.size();
		r.push_back(5);

		addr = (stream[3] << 8) | (stream[4]);
		size = stream[5];

		if ((addr + size) >= 0xffff)
			size = 0xffff - addr;

		for (int i = 0; i < size; i++)
			r.push_back(SRAM[addr++]);

		r[pos] += size;
		//logmsg("SRAM READ %d : %x\n", size, addr);

		r.push_back(0x70);
		r.push_back(0x42);
		break;
	}

	case 0xfe:
		r.push_back(0x76);
		r.push_back(0xfe);
		r.push_back(0x06);
		if (stream[3])
			r.push_back(0x01);
		else
			r.push_back(0);
		r.push_back(0x70);
		r.push_back(0x42);
		break;

	case 0x01:
		r.push_back(0x76);
		r.push_back(0x01);
		r.push_back(0x06);
		r.push_back(0x00);
		r.push_back(0x70);
		r.push_back(0x42);
		isAddressedExBoard = 1;
		break;
	case 0x08: // error
		isAddressedExBoard = 0;
	default:
		break;
	}

	BYTE *pdst = dst;
	unsigned i = 0;
	unsigned maxv = r.size();

	if (maxv > dstsize)
		maxv = dstsize;

	for (i = 0; i < maxv; i++)
		*pdst++ = r[i];

	unsigned sz = r.size();
	//r.clear();
	return sz;
}

using namespace std::string_literals;
static std::map<HANDLE, std::deque<BYTE>> g_replyBuffers;
void AddCommOverride(HANDLE hFile);

static BOOL __stdcall ReadFileWrapExBoard(HANDLE hFile,
	LPVOID lpBuffer,
	DWORD nNumberOfBytesToRead,
	LPDWORD lpNumberOfBytesRead,
	LPOVERLAPPED lpOverlapped)
{
	if (hFile == (HANDLE)0x8001)
	{
		OutputDebugStringA("COM1 READ");
		auto& outQueue = g_replyBuffers[hFile];

		int toRead = min(outQueue.size(), nNumberOfBytesToRead);

		std::copy(outQueue.begin(), outQueue.begin() + toRead, reinterpret_cast<uint8_t*>(lpBuffer));
		outQueue.erase(outQueue.begin(), outQueue.begin() + toRead);

		*lpNumberOfBytesRead = toRead;

		return TRUE;
	}
	return ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
}


static HANDLE __stdcall CreateFileAWrapExBoard(LPCSTR lpFileName,
	DWORD dwDesiredAccess,
	DWORD dwShareMode,
	LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	DWORD dwCreationDisposition,
	DWORD dwFlagsAndAttributes,
	HANDLE hTemplateFile)
{
	if (strnicmp(lpFileName, "D:\\", 3) == 0)
	{
		if (GetFileAttributesA(lpFileName) == INVALID_FILE_ATTRIBUTES)
		{
			wchar_t pathRoot[MAX_PATH];
			GetModuleFileNameW(GetModuleHandle(nullptr), pathRoot, _countof(pathRoot));

			wcsrchr(pathRoot, L'\\')[0] = L'\0';

			// assume just ASCII
			std::string fn = lpFileName;
			std::wstring wfn(fn.begin(), fn.end());

			CreateDirectoryW((pathRoot + L"\\TeknoParrot\\"s).c_str(), nullptr);

			return CreateFileW((pathRoot + L"\\TeknoParrot\\"s + wfn.substr(3)).c_str(),
				dwDesiredAccess,
				dwShareMode,
				lpSecurityAttributes,
				dwCreationDisposition,
				dwFlagsAndAttributes,
				hTemplateFile);
		}
	}

	if (strncmp(lpFileName, "COM1", 4) == 0)
	{
		OutputDebugStringA("COM1 HOOK");
		HANDLE hFile = (HANDLE)0x8001;

		AddCommOverride(hFile);

		return hFile;
	}

	// Some Daemon Bride dumps store the eX-Board FlashRom payload directly
	// beside DB1.exe (for example font/font_dat.tbl).  The original executable
	// still prefixes those reads with FlashRom/ and otherwise spins forever on
	// the first missing font table.  Preserve a real FlashRom tree when present
	// and fall back only when the exact unprefixed file exists.
	if (const char* redirectedPath = ResolveDaemonBrideFlashRomPath(lpFileName))
	{
		return CreateFileA(redirectedPath,
			dwDesiredAccess,
			dwShareMode,
			lpSecurityAttributes,
			dwCreationDisposition,
			dwFlagsAndAttributes,
			hTemplateFile);
	}

	return CreateFileA(lpFileName,
		dwDesiredAccess,
		dwShareMode,
		lpSecurityAttributes,
		dwCreationDisposition,
		dwFlagsAndAttributes,
		hTemplateFile);
}

BOOL __stdcall WriteFileWrapExBoard(HANDLE hFile,
	LPVOID lpBuffer,
	DWORD nNumberOfBytesToWrite,
	LPDWORD lpNumberOfBytesWritten,
	LPOVERLAPPED lpOverlapped)
{
	if (hFile == (HANDLE)0x8001)
	{
		OutputDebugStringA("COM1 WRITE");
		static BYTE rbuffer[1024];
		DWORD sz = process_streamExBoard((LPBYTE)lpBuffer, nNumberOfBytesToWrite, rbuffer, 1024);
		if (sz != 1) {
			for (DWORD i = 0; i < sz; i++)
				g_replyBuffers[hFile].push_back(rbuffer[i]);
		}

		*lpNumberOfBytesWritten = nNumberOfBytesToWrite;

		return TRUE;
	}
	return WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}

extern int* ffbOffset;

BOOL __stdcall ClearCommErrorWrapExBoard(HANDLE hFile, LPDWORD lpErrors, LPCOMSTAT lpStat)
{
	if (hFile != (HANDLE)0x8001)
	{
		return ClearCommError(hFile, lpErrors, lpStat);
	}

	if(lpStat)
	{
		OutputDebugStringA("CLEAR COMM ERROR COM1");
		if(!g_replyBuffers[hFile].empty())
		{
			lpStat->cbInQue = g_replyBuffers[hFile].size();
		}
		else
		{
			lpStat->cbInQue = 0;
		}

		if (is_addressedExBoard())
		{

			lpStat->cbInQue += 8;

			g_replyBuffers[hFile].push_back(0x76);
			g_replyBuffers[hFile].push_back(0xFD);
			g_replyBuffers[hFile].push_back(0x08);
			g_replyBuffers[hFile].push_back(*ffbOffset & 0xFF); // Control Byte 1
			g_replyBuffers[hFile].push_back(*ffbOffset >> 8 & 0xFF); // Control Byte 2
			g_replyBuffers[hFile].push_back(*ffbOffset >> 16 & 0xFF); // Control Byte 3
			g_replyBuffers[hFile].push_back(*ffbOffset >> 24 & 0xFF); // Control Byte 4
			g_replyBuffers[hFile].push_back(0x42);
		}
	}

	return true;
}

BOOL __stdcall GetCommModemStatusWrapExBoard(HANDLE hFile, LPDWORD lpModemStat)
{
	if (hFile != (HANDLE)0x8001) {
		return GetCommModemStatus(hFile, lpModemStat);
	}

	if (is_addressedExBoard())
		*lpModemStat = 0x10;
	else
		*lpModemStat = 0;

	return TRUE;
}

BOOL WINAPI CloseHandleWrapExBoard(
	_In_ HANDLE hObject
)
{
	if (hObject == (HANDLE)0x8001)
	{
		reset_addressedExBoard();
		return TRUE;
	}
	return CloseHandle(hObject);
}

int __stdcall GetKeyLicenseWrap(void)
{
	return 1;
}

LONG __stdcall ChangeDisplaySettingsAWrap(DEVMODEA *lpDevMode, DWORD dwflags)
{
	if (ToBool(config["General"]["Windowed"]))
	{
		return DISP_CHANGE_SUCCESSFUL;
	}
	lpDevMode->dmBitsPerPel = 32;
	lpDevMode->dmDisplayFrequency = 60;

	return ChangeDisplaySettingsA(lpDevMode, CDS_FULLSCREEN);
}

BOOL __stdcall ExitWindowsExWrap(UINT uFlags, DWORD dwReason)
{
	return FALSE;
}

SHORT __stdcall GetAsyncKeyStateWrap(int vKey)
{
	return 0;
}

BOOL __stdcall SetWindowPosWrap(HWND hWnd,
	HWND hWndInsertAfter,
	int X,
	int Y,
	int cx,
	int cy,
	UINT uFlags)
{
	return TRUE;
}

HWND __stdcall CreateWindowExWWrap(DWORD dwExStyle,
	LPCWSTR lpClassName,
	LPCWSTR lpWindowName,
	DWORD dwStyle,
	int x,
	int y,
	int nWidth,
	int nHeight,
	HWND hWndParent,
	HMENU hMenu,
	HINSTANCE hInstance,
	LPVOID lpParam)
{
	dwExStyle = 0;
	if (ToBool(config["General"]["Windowed"]))
	{
		dwStyle = WS_OVERLAPPEDWINDOW;

		RECT r;
		r.bottom = nHeight;
		r.top = 0;
		r.right = nWidth;
		r.left = 0;
		AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

		return CreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, r.right, r.bottom, hWndParent,
			hMenu, hInstance, lpParam);
	}

	dwStyle = WS_EX_TOPMOST | WS_POPUP;
	x = 0;
	y = 0;
	return CreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight, hWndParent,
		hMenu, hInstance, lpParam);

}

HWND __stdcall CreateWindowExAWrap(DWORD dwExStyle,
	LPCSTR lpClassName,
	LPCSTR lpWindowName,
	DWORD dwStyle,
	int x,
	int y,
	int nWidth,
	int nHeight,
	HWND hWndParent,
	HMENU hMenu,
	HINSTANCE hInstance,
	LPVOID lpParam)
{	
	dwExStyle = 0;
	if (ToBool(config["General"]["Windowed"]))
	{
		dwStyle = WS_OVERLAPPEDWINDOW;

		RECT r;
		r.right = nWidth;
		r.bottom = nHeight;
		r.top = 0;
		r.left = 0;
		AdjustWindowRect(&r, dwStyle, FALSE);

		nWidth = r.right - r.left;
		nHeight = r.bottom - r.top;

		return CreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle, 0, 0, nWidth, nHeight, hWndParent,
			hMenu, hInstance, lpParam);
	}

	dwStyle = WS_EX_TOPMOST | WS_POPUP;
	x = 0;
	y = 0;
	return CreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight, hWndParent,
		hMenu, hInstance, lpParam);
}

static InitFunction ExBoardGenericFunc([]()
{
	if (IsDaemonBrideExBoard() && IsExBoardRunningUnderWine())
	{
		DaemonBrideDirect3DCreate9Original = iatHook(
			"d3d9.dll",
			DaemonBrideDirect3DCreate9Hook,
			"Direct3DCreate9");
	}

	if (ShouldForceSoftwareDirectSoundExBoard())
	{
		const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
		const uintptr_t bufferFlagsAddress = imageBase + 0x8C048;
		const uint32_t originalBufferFlags =
			*reinterpret_cast<const uint32_t*>(bufferFlagsAddress);
		if (originalBufferFlags == 0x000580E0)
		{
			// Wine exposes no hardware mixing buffers.  Avoid thousands of
			// deferred buffer realizations by explicitly selecting software.
			injector::WriteMemory<uint32_t>(
				bufferFlagsAddress, 0x000180E8, true);
		}
	}
	iatHook("kernel32.dll", CreateFileAWrapExBoard, "CreateFileA");
	iatHook("kernel32.dll", ReadFileWrapExBoard, "ReadFile");
	iatHook("kernel32.dll", WriteFileWrapExBoard, "WriteFile");
	iatHook("kernel32.dll", ClearCommErrorWrapExBoard, "ClearCommError");
	iatHook("kernel32.dll", GetCommModemStatusWrapExBoard, "GetCommModemStatus");
	iatHook("kernel32.dll", CloseHandleWrapExBoard, "CloseHandle");
	iatHook("IpgExKey.dll", GetKeyLicenseWrap, "_GetKeyLicense@0");
	iatHook("user32.dll", ChangeDisplaySettingsAWrap, "ChangeDisplaySettingsA");
	iatHook("user32.dll", ExitWindowsExWrap, "ExitWindowsEx");
	iatHook("user32.dll", GetAsyncKeyStateWrap, "GetAsyncKeyState");
	iatHook("user32.dll", SetWindowPosWrap, "SetWindowPos");
	iatHook("user32.dll", CreateWindowExWWrap, "CreateWindowExW");
	iatHook("user32.dll", CreateWindowExAWrap, "CreateWindowExA");

	SRAM_load();

}, GameID::ExBoardGeneric);
#endif

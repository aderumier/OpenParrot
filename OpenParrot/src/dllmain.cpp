#include <StdInc.h>
#include <Utility/InitFunction.h>
#include <Utility/GameDetect.h>
linb::ini config;
#include <Functions/Global.h>

#pragma optimize("", off)

static void RunMain();

static BYTE originalCode[20];
extern "C" PBYTE originalEP = 0;

#if _M_IX86
static char ghaInitTracePath[MAX_PATH] = {};

static bool IsGhaInitDiagnosticsEnabled()
{
	char value[8] = {};
	const DWORD length = GetEnvironmentVariableA(
		"TP_GHA_LOCAL_DIAGNOSTICS",
		value,
		_countof(value));
	return length > 0 &&
		length < _countof(value) &&
		value[0] == '1';
}

static void AppendGhaInitTrace(const char* line)
{
	if (ghaInitTracePath[0] == '\0')
		return;

	HANDLE file = CreateFileA(
		ghaInitTracePath,
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
	CloseHandle(file);
}

static void WriteGhaInitTrace(HMODULE module)
{
	if (!IsGhaInitDiagnosticsEnabled())
		return;

	char modulePath[MAX_PATH] = {};
	if (GetModuleFileNameA(
			module,
			modulePath,
			static_cast<DWORD>(std::size(modulePath))) == 0)
	{
		return;
	}

	char* separator = strrchr(modulePath, '\\');
	if (separator == nullptr)
		return;
	strcpy_s(
		separator + 1,
		MAX_PATH - static_cast<size_t>(separator + 1 - modulePath),
		"OpenParrotGHAInitTrace.log");
	strcpy_s(ghaInitTracePath, modulePath);

	char line[256] = {};
	sprintf_s(
		line,
		"DllMain attach managed=%d direct=%d remote=%d entryDelay=%d\r\n",
		getenv("TP_LOADER_MANAGED_INIT") != nullptr ? 1 : 0,
		getenv("TP_DIRECTHOOK") != nullptr ? 1 : 0,
		getenv("TP_REMOTETHREAD") != nullptr ? 1 : 0,
		getenv("TP_ENTRYPOINT_REMOTETHREAD_MS") != nullptr ? 1 : 0);

	HANDLE file = CreateFileA(
		modulePath,
		GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
	CloseHandle(file);
}
#endif

#ifdef _M_AMD64
extern "C" void Main_DoResume();
#endif

static void Main_DoInit()
{
#if _M_IX86
	AppendGhaInitTrace("Main_DoInit entered\r\n");
#endif
	RunMain();

	DWORD oldProtect;
	VirtualProtect(originalEP, 20, PAGE_EXECUTE_READWRITE, &oldProtect);

	memcpy(originalEP, &originalCode, sizeof(originalCode));

	VirtualProtect(originalEP, 20, oldProtect, &oldProtect);

#if _M_IX86
	__asm jmp originalEP
#elif defined(_M_AMD64)
	Main_DoResume();
#endif
}

static void Main_SetSafeInit()
{
	// find the entry point for the executable process, set page access, and replace the EP
	HMODULE hModule = GetModuleHandle(NULL);

	if (hModule)
	{
		PIMAGE_DOS_HEADER header = (PIMAGE_DOS_HEADER)hModule;
		PIMAGE_NT_HEADERS ntHeader = (PIMAGE_NT_HEADERS)((DWORD_PTR)hModule + header->e_lfanew);

		// back up original code
		PBYTE ep = (PBYTE)((DWORD_PTR)hModule + ntHeader->OptionalHeader.AddressOfEntryPoint);
		memcpy(originalCode, ep, sizeof(originalCode));

		DWORD oldProtect;
		const BOOL protectResult =
			VirtualProtect(ep, 20, PAGE_EXECUTE_READWRITE, &oldProtect);

#ifdef _M_IX86
		char trace[256] = {};
		sprintf_s(
			trace,
			"Main_SetSafeInit image=%p ep=%p protect=%d error=%lu original=%02X%02X%02X%02X%02X\r\n",
			hModule,
			ep,
			protectResult ? 1 : 0,
			protectResult ? ERROR_SUCCESS : GetLastError(),
			originalCode[0],
			originalCode[1],
			originalCode[2],
			originalCode[3],
			originalCode[4]);
		AppendGhaInitTrace(trace);
		if (!protectResult)
			return;

		// patch to call our EP
		int newEP = (int)Main_DoInit - ((int)ep + 5);
		ep[0] = 0xE9; // for some reason this doesn't work properly when run under the debugger
		memcpy(&ep[1], &newEP, 4);
#elif defined(_M_AMD64)
		ep[0] = 0x48;
		ep[1] = 0xB8;
		*(uint64_t *)(ep + 2) = (uint64_t)Main_DoInit;
		ep[10] = 0xFF;
		ep[11] = 0xE0;
#endif

		VirtualProtect(ep, 20, oldProtect, &oldProtect);

		originalEP = ep;
	}
}

static void RunMain()
{
#if _M_IX86
	AppendGhaInitTrace("RunMain entered\r\n");
#endif
	static bool initialized;

	if (initialized)
	{
		return;
	}

	initialized = true;

	if (!config.load_file("teknoparrot.ini"))
	{
		//MessageBoxA(NULL, V("Failed to open config.ini"), V("TeknoParrot",) MB_OK);
		//std::_Exit(0);
	}

	GameDetect::DetectCurrentGame();
#if _M_IX86
	char trace[128] = {};
	sprintf_s(
		trace,
		"RunMain detected game=%d\r\n",
		static_cast<int>(GameDetect::currentGame));
	AppendGhaInitTrace(trace);
#endif
	InitFunction::RunFunctions(GameID::Global);
	InitFunction::RunFunctions(GameDetect::currentGame);
}

void* (*g_makeCall)(void* call);
#if _M_IX86
extern "C" __declspec(dllexport) void InitLinux(void* (*makeCall)(void*))
{
	g_makeCall = makeCall;

	if (!config.load_file("teknoparrot.ini"))
	{
		//MessageBoxA(NULL, V("Failed to open config.ini"), V("TeknoParrot",) MB_OK);
		//std::_Exit(0);
	}

	GameDetect::DetectCurrentLinuxGame();
	InitFunction::RunFunctions(GameID::Global);
	InitFunction::RunFunctions(GameID::LinuxEmulation);
	InitFunction::RunFunctions(GameDetect::currentGame);
}
#endif

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
#if _M_IX86
		WriteGhaInitTrace(hModule);
#endif
		// Wine/Box64 can load us from a remote thread after the game's real
		// entry point has been parked by OpenParrotLoader.  In that mode DllMain
		// must remain inert: the loader invokes PrepareSafeInit after LoadLibrary
		// has returned and the target loader lock has been released.
		if (getenv("TP_LOADER_MANAGED_INIT") != nullptr)
		{
			return TRUE;
		}

		if (getenv("TP_DIRECTHOOK") != nullptr)
		{
			RunMain();
			return TRUE;
		}
#ifdef DEVMODE
		RunMain();
#else
		Main_SetSafeInit();
#endif
	}
	return TRUE; // false
}

extern "C" __declspec(dllexport) void InitializeASI()
{
	RunMain();
}

extern "C" __declspec(dllexport) DWORD WINAPI PrepareSafeInit(LPVOID)
{
	Main_SetSafeInit();
	return 1;
}
#pragma optimize("", on)

#include <StdInc.h>
#include "Utility/GameDetect.h"
#include "Utility/InitFunction.h"
#include "Functions/Nesica_Libs/CryptoPipe.h"
#include "Functions/Nesica_Libs/FastIoEmu.h"
#include "Functions/Nesica_Libs/RfidEmu.h"
#include "Functions/Nesica_Libs/NesysEmu.h"
#include "Functions/Nesica_Libs/RegHooks.h"
#include "Utility/Hooking.Patterns.h"
#include "Utility/WineCompat.h"
#include <Functions/Global.h>
#include <d3d9.h>
#include <iphlpapi.h>

namespace
{
#if _M_IX86
	uintptr_t akaiMovieSeekingGuardContinue = 0;
	uintptr_t akaiMovieSeekingGuardReturn = 0;
	decltype(&GetAdaptersInfo) grooveGetAdaptersInfoOri = nullptr;
	decltype(&CreateFileA) enEinsCreateFileAOri = nullptr;
	decltype(&CreateFileW) enEinsCreateFileWOri = nullptr;
	decltype(&OutputDebugStringA) enEinsOutputDebugStringAOri = nullptr;
	decltype(&MessageBoxA) enEinsMessageBoxAOri = nullptr;
	decltype(&Direct3DCreate9) enEinsDirect3DCreate9Ori = nullptr;
	using EnEinsCreateDeviceFunction = HRESULT(WINAPI*)(
		IDirect3D9*,
		UINT,
		D3DDEVTYPE,
		HWND,
		DWORD,
		D3DPRESENT_PARAMETERS*,
		IDirect3DDevice9**);
	using EnEinsBeginSceneFunction = HRESULT(WINAPI*)(IDirect3DDevice9*);
	using EnEinsClearFunction = HRESULT(WINAPI*)(
		IDirect3DDevice9*,
		DWORD,
		const D3DRECT*,
		DWORD,
		D3DCOLOR,
		float,
		DWORD);
	using EnEinsDrawPrimitiveFunction = HRESULT(WINAPI*)(
		IDirect3DDevice9*,
		D3DPRIMITIVETYPE,
		UINT,
		UINT);
	using EnEinsDrawIndexedPrimitiveFunction = HRESULT(WINAPI*)(
		IDirect3DDevice9*,
		D3DPRIMITIVETYPE,
		INT,
		UINT,
		UINT,
		UINT,
		UINT);
	using EnEinsDrawPrimitiveUpFunction = HRESULT(WINAPI*)(
		IDirect3DDevice9*,
		D3DPRIMITIVETYPE,
		UINT,
		const void*,
		UINT);
	using EnEinsDrawIndexedPrimitiveUpFunction = HRESULT(WINAPI*)(
		IDirect3DDevice9*,
		D3DPRIMITIVETYPE,
		UINT,
		UINT,
		UINT,
		const void*,
		D3DFORMAT,
		const void*,
		UINT);
	using EnEinsPresentFunction = HRESULT(WINAPI*)(
		IDirect3DDevice9*,
		const RECT*,
		const RECT*,
		HWND,
		const RGNDATA*);
	EnEinsCreateDeviceFunction enEinsCreateDeviceOri = nullptr;
	EnEinsBeginSceneFunction enEinsBeginSceneOri = nullptr;
	EnEinsClearFunction enEinsClearOri = nullptr;
	EnEinsDrawPrimitiveFunction enEinsDrawPrimitiveOri = nullptr;
	EnEinsDrawIndexedPrimitiveFunction enEinsDrawIndexedPrimitiveOri = nullptr;
	EnEinsDrawPrimitiveUpFunction enEinsDrawPrimitiveUpOri = nullptr;
	EnEinsDrawIndexedPrimitiveUpFunction enEinsDrawIndexedPrimitiveUpOri =
		nullptr;
	EnEinsPresentFunction enEinsPresentOri = nullptr;
	volatile LONG enEinsFileCallCount = 0;
	volatile LONG enEinsMessageCount = 0;
	volatile LONG enEinsCreateDeviceCount = 0;
	volatile LONG enEinsBeginSceneCount = 0;
	volatile LONG enEinsClearCount = 0;
	volatile LONG enEinsDrawPrimitiveCount = 0;
	volatile LONG enEinsDrawIndexedPrimitiveCount = 0;
	volatile LONG enEinsDrawPrimitiveUpCount = 0;
	volatile LONG enEinsDrawIndexedPrimitiveUpCount = 0;
	volatile LONG enEinsPresentCount = 0;

	bool IsGrooveUsableIpv4(const char* address)
	{
		return address != nullptr &&
			address[0] != '\0' &&
			strcmp(address, "0.0.0.0") != 0 &&
			strncmp(address, "127.", 4) != 0 &&
			strncmp(address, "169.254.", 8) != 0;
	}

	bool HasGrooveMacAddress(const IP_ADAPTER_INFO* adapter)
	{
		if (adapter == nullptr || adapter->AddressLength < 6)
			return false;

		for (UINT index = 0; index < 6; ++index)
		{
			if (adapter->Address[index] != 0)
				return true;
		}

		return false;
	}

	ULONG WINAPI GrooveGetAdaptersInfo(
		PIP_ADAPTER_INFO adapterInfo,
		PULONG outputBufferLength)
	{
		const ULONG result =
			grooveGetAdaptersInfoOri(adapterInfo, outputBufferLength);
		if (result != ERROR_SUCCESS || adapterInfo == nullptr)
			return result;

		const std::string configuredAddress =
			config["General"]["NetworkAdapterIP"];
		PIP_ADAPTER_INFO selectedAdapter = nullptr;
		int selectedScore = -1;
		for (PIP_ADAPTER_INFO candidate = adapterInfo;
			candidate != nullptr;
			candidate = candidate->Next)
		{
			const char* candidateAddress =
				candidate->IpAddressList.IpAddress.String;
			if (!IsGrooveUsableIpv4(candidateAddress))
				continue;

			int score = 0;
			if (!configuredAddress.empty() &&
				configuredAddress == candidateAddress)
				score += 100;
			if (HasGrooveMacAddress(candidate))
				score += 20;
			if (IsGrooveUsableIpv4(
				candidate->GatewayList.IpAddress.String))
				score += 10;

			if (score > selectedScore)
			{
				selectedAdapter = candidate;
				selectedScore = score;
			}
		}

		// Groove Coaster chooses the lexicographically smallest MAC without
		// rejecting disconnected pseudo adapters. Wine and Windows can expose
		// an all-zero adapter first, leaving the game's pre-render network-ready
		// word at zero forever. Return only the best usable IPv4 adapter so the
		// game's original selection and network initialization can continue.
		if (selectedAdapter != nullptr)
		{
			const bool selectedCurrentIpIsPrimary =
				selectedAdapter->CurrentIpAddress ==
				&selectedAdapter->IpAddressList;
			const IP_ADAPTER_INFO selectedCopy = *selectedAdapter;
			*adapterInfo = selectedCopy;
			adapterInfo->Next = nullptr;
			if (selectedCurrentIpIsPrimary)
				adapterInfo->CurrentIpAddress =
					&adapterInfo->IpAddressList;

			if (!HasGrooveMacAddress(adapterInfo))
			{
				static const BYTE fallbackMac[6] =
					{ 0x02, 0x54, 0x50, 0x47, 0x43, 0x32 };
				adapterInfo->AddressLength = _countof(fallbackMac);
				memcpy(
					adapterInfo->Address,
					fallbackMac,
					sizeof(fallbackMac));
			}
		}

		return result;
	}

	bool IsEnEinsDiagnosticsEnabled()
	{
		char value[8] = {};
		const DWORD length = GetEnvironmentVariableA(
			"TP_ENEINS_DIAGNOSTICS",
			value,
			_countof(value));
		return length > 0 &&
			length < _countof(value) &&
			value[0] == '1';
	}

	void WriteEnEinsTrace(
		const char* section,
		const char* prefix,
		const LONG sequence,
		const char* value)
	{
		char key[48] = {};
		sprintf_s(key, "%s%ld", prefix, sequence);
		char sanitized[768] = {};
		strncpy_s(sanitized, value == nullptr ? "<null>" : value, _TRUNCATE);
		for (char* cursor = sanitized; *cursor != '\0'; ++cursor)
		{
			if (*cursor == '\r' || *cursor == '\n')
				*cursor = ' ';
		}
		WritePrivateProfileStringA(
			section,
			key,
			sanitized,
			".\\EnEinsDiagnostic.ini");
	}

	HANDLE WINAPI EnEinsCreateFileAHook(
		LPCSTR fileName,
		DWORD desiredAccess,
		DWORD shareMode,
		LPSECURITY_ATTRIBUTES securityAttributes,
		DWORD creationDisposition,
		DWORD flagsAndAttributes,
		HANDLE templateFile)
	{
		SetLastError(ERROR_SUCCESS);
		const HANDLE result = enEinsCreateFileAOri(
			fileName,
			desiredAccess,
			shareMode,
			securityAttributes,
			creationDisposition,
			flagsAndAttributes,
			templateFile);
		const DWORD error = GetLastError();
		const LONG call = InterlockedIncrement(&enEinsFileCallCount);
		if (call <= 256)
		{
			char value[640] = {};
			sprintf_s(
				value,
				"result=%08X error=%lu access=%08X disposition=%lu path=%s",
				reinterpret_cast<uintptr_t>(result),
				static_cast<unsigned long>(error),
				desiredAccess,
				static_cast<unsigned long>(creationDisposition),
				fileName == nullptr ? "<null>" : fileName);
			WriteEnEinsTrace("Files", "A", call, value);
		}
		SetLastError(error);
		return result;
	}

	HANDLE WINAPI EnEinsCreateFileWHook(
		LPCWSTR fileName,
		DWORD desiredAccess,
		DWORD shareMode,
		LPSECURITY_ATTRIBUTES securityAttributes,
		DWORD creationDisposition,
		DWORD flagsAndAttributes,
		HANDLE templateFile)
	{
		SetLastError(ERROR_SUCCESS);
		const HANDLE result = enEinsCreateFileWOri(
			fileName,
			desiredAccess,
			shareMode,
			securityAttributes,
			creationDisposition,
			flagsAndAttributes,
			templateFile);
		const DWORD error = GetLastError();
		const LONG call = InterlockedIncrement(&enEinsFileCallCount);
		if (call <= 256)
		{
			char path[520] = {};
			if (fileName != nullptr)
			{
				WideCharToMultiByte(
					CP_UTF8,
					0,
					fileName,
					-1,
					path,
					_countof(path),
					nullptr,
					nullptr);
			}
			char value[640] = {};
			sprintf_s(
				value,
				"result=%08X error=%lu access=%08X disposition=%lu path=%s",
				reinterpret_cast<uintptr_t>(result),
				static_cast<unsigned long>(error),
				desiredAccess,
				static_cast<unsigned long>(creationDisposition),
				fileName == nullptr ? "<null>" : path);
			WriteEnEinsTrace("Files", "W", call, value);
		}
		SetLastError(error);
		return result;
	}

	void WINAPI EnEinsOutputDebugStringAHook(LPCSTR message)
	{
		const LONG sequence = InterlockedIncrement(&enEinsMessageCount);
		if (sequence <= 128)
			WriteEnEinsTrace("Messages", "Debug", sequence, message);
		enEinsOutputDebugStringAOri(message);
	}

	int WINAPI EnEinsMessageBoxAHook(
		HWND window,
		LPCSTR text,
		LPCSTR caption,
		UINT type)
	{
		const LONG sequence = InterlockedIncrement(&enEinsMessageCount);
		char value[768] = {};
		sprintf_s(
			value,
			"type=%08X caption=%s text=%s",
			type,
			caption == nullptr ? "<null>" : caption,
			text == nullptr ? "<null>" : text);
		WriteEnEinsTrace("Messages", "Box", sequence, value);
		return enEinsMessageBoxAOri(window, text, caption, type);
	}

	void InstallEnEinsVtableHook(
		void** vtable,
		const size_t index,
		void* hook,
		void** original)
	{
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

	HRESULT WINAPI EnEinsBeginSceneHook(IDirect3DDevice9* device)
	{
		InterlockedIncrement(&enEinsBeginSceneCount);
		return enEinsBeginSceneOri(device);
	}

	HRESULT WINAPI EnEinsClearHook(
		IDirect3DDevice9* device,
		DWORD count,
		const D3DRECT* rects,
		DWORD flags,
		D3DCOLOR color,
		float depth,
		DWORD stencil)
	{
		InterlockedIncrement(&enEinsClearCount);
		return enEinsClearOri(
			device,
			count,
			rects,
			flags,
			color,
			depth,
			stencil);
	}

	HRESULT WINAPI EnEinsDrawPrimitiveHook(
		IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType,
		UINT startVertex,
		UINT primitiveCount)
	{
		InterlockedIncrement(&enEinsDrawPrimitiveCount);
		return enEinsDrawPrimitiveOri(
			device,
			primitiveType,
			startVertex,
			primitiveCount);
	}

	HRESULT WINAPI EnEinsDrawIndexedPrimitiveHook(
		IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType,
		INT baseVertexIndex,
		UINT minimumVertexIndex,
		UINT numberOfVertices,
		UINT startIndex,
		UINT primitiveCount)
	{
		InterlockedIncrement(&enEinsDrawIndexedPrimitiveCount);
		return enEinsDrawIndexedPrimitiveOri(
			device,
			primitiveType,
			baseVertexIndex,
			minimumVertexIndex,
			numberOfVertices,
			startIndex,
			primitiveCount);
	}

	HRESULT WINAPI EnEinsDrawPrimitiveUpHook(
		IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType,
		UINT primitiveCount,
		const void* vertexStreamZeroData,
		UINT vertexStreamZeroStride)
	{
		InterlockedIncrement(&enEinsDrawPrimitiveUpCount);
		return enEinsDrawPrimitiveUpOri(
			device,
			primitiveType,
			primitiveCount,
			vertexStreamZeroData,
			vertexStreamZeroStride);
	}

	HRESULT WINAPI EnEinsDrawIndexedPrimitiveUpHook(
		IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType,
		UINT minimumVertexIndex,
		UINT numberOfVertices,
		UINT primitiveCount,
		const void* indexData,
		D3DFORMAT indexDataFormat,
		const void* vertexStreamZeroData,
		UINT vertexStreamZeroStride)
	{
		InterlockedIncrement(&enEinsDrawIndexedPrimitiveUpCount);
		return enEinsDrawIndexedPrimitiveUpOri(
			device,
			primitiveType,
			minimumVertexIndex,
			numberOfVertices,
			primitiveCount,
			indexData,
			indexDataFormat,
			vertexStreamZeroData,
			vertexStreamZeroStride);
	}

	HRESULT WINAPI EnEinsPresentHook(
		IDirect3DDevice9* device,
		const RECT* sourceRect,
		const RECT* destinationRect,
		HWND destinationWindowOverride,
		const RGNDATA* dirtyRegion)
	{
		const HRESULT result = enEinsPresentOri(
			device,
			sourceRect,
			destinationRect,
			destinationWindowOverride,
			dirtyRegion);
		const LONG present = InterlockedIncrement(&enEinsPresentCount);
		if (present == 1 || present == 60 || present == 600)
		{
			char key[32] = {};
			sprintf_s(key, "Present%ld", present);
			char value[256] = {};
			sprintf_s(
				value,
				"result=%08X begin=%ld clear=%ld draw=%ld indexed=%ld up=%ld indexedUp=%ld",
				result,
				enEinsBeginSceneCount,
				enEinsClearCount,
				enEinsDrawPrimitiveCount,
				enEinsDrawIndexedPrimitiveCount,
				enEinsDrawPrimitiveUpCount,
				enEinsDrawIndexedPrimitiveUpCount);
			WritePrivateProfileStringA(
				"Renderer",
				key,
				value,
				".\\EnEinsDiagnostic.ini");
		}
		return result;
	}

	void InstallEnEinsDeviceDiagnostics(IDirect3DDevice9* device)
	{
		void** vtable = *reinterpret_cast<void***>(device);
		InstallEnEinsVtableHook(
			vtable,
			17,
			reinterpret_cast<void*>(EnEinsPresentHook),
			reinterpret_cast<void**>(&enEinsPresentOri));
		InstallEnEinsVtableHook(
			vtable,
			41,
			reinterpret_cast<void*>(EnEinsBeginSceneHook),
			reinterpret_cast<void**>(&enEinsBeginSceneOri));
		InstallEnEinsVtableHook(
			vtable,
			43,
			reinterpret_cast<void*>(EnEinsClearHook),
			reinterpret_cast<void**>(&enEinsClearOri));
		InstallEnEinsVtableHook(
			vtable,
			81,
			reinterpret_cast<void*>(EnEinsDrawPrimitiveHook),
			reinterpret_cast<void**>(&enEinsDrawPrimitiveOri));
		InstallEnEinsVtableHook(
			vtable,
			82,
			reinterpret_cast<void*>(EnEinsDrawIndexedPrimitiveHook),
			reinterpret_cast<void**>(&enEinsDrawIndexedPrimitiveOri));
		InstallEnEinsVtableHook(
			vtable,
			83,
			reinterpret_cast<void*>(EnEinsDrawPrimitiveUpHook),
			reinterpret_cast<void**>(&enEinsDrawPrimitiveUpOri));
		InstallEnEinsVtableHook(
			vtable,
			84,
			reinterpret_cast<void*>(EnEinsDrawIndexedPrimitiveUpHook),
			reinterpret_cast<void**>(&enEinsDrawIndexedPrimitiveUpOri));
	}

	HRESULT WINAPI EnEinsCreateDeviceHook(
		IDirect3D9* direct3D,
		UINT adapter,
		D3DDEVTYPE deviceType,
		HWND focusWindow,
		DWORD behaviorFlags,
		D3DPRESENT_PARAMETERS* presentationParameters,
		IDirect3DDevice9** returnedDevice)
	{
		const HRESULT result = enEinsCreateDeviceOri(
			direct3D,
			adapter,
			deviceType,
			focusWindow,
			behaviorFlags,
			presentationParameters,
			returnedDevice);
		const LONG call = InterlockedIncrement(&enEinsCreateDeviceCount);
		char key[32] = {};
		sprintf_s(key, "CreateDevice%ld", call);
		char value[320] = {};
		sprintf_s(
			value,
			"result=%08X adapter=%u type=%u behavior=%08X windowed=%u width=%u height=%u format=%u swap=%u multisample=%u",
			result,
			adapter,
			static_cast<unsigned>(deviceType),
			behaviorFlags,
			presentationParameters == nullptr ?
				0 : presentationParameters->Windowed,
			presentationParameters == nullptr ?
				0 : presentationParameters->BackBufferWidth,
			presentationParameters == nullptr ?
				0 : presentationParameters->BackBufferHeight,
			presentationParameters == nullptr ?
				0 : presentationParameters->BackBufferFormat,
			presentationParameters == nullptr ?
				0 : presentationParameters->SwapEffect,
			presentationParameters == nullptr ?
				0 : presentationParameters->MultiSampleType);
		WritePrivateProfileStringA(
			"Renderer",
			key,
			value,
			".\\EnEinsDiagnostic.ini");
		if (SUCCEEDED(result) &&
			returnedDevice != nullptr &&
			*returnedDevice != nullptr)
			InstallEnEinsDeviceDiagnostics(*returnedDevice);
		return result;
	}

	IDirect3D9* WINAPI EnEinsDirect3DCreate9Hook(UINT sdkVersion)
	{
		IDirect3D9* result = enEinsDirect3DCreate9Ori(sdkVersion);
		char value[96] = {};
		sprintf_s(
			value,
			"sdk=%u result=%08X",
			sdkVersion,
			reinterpret_cast<uintptr_t>(result));
		WritePrivateProfileStringA(
			"Renderer",
			"Direct3DCreate9",
			value,
			".\\EnEinsDiagnostic.ini");
		if (result != nullptr)
		{
			void** vtable = *reinterpret_cast<void***>(result);
			InstallEnEinsVtableHook(
				vtable,
				16,
				reinterpret_cast<void*>(EnEinsCreateDeviceHook),
				reinterpret_cast<void**>(&enEinsCreateDeviceOri));
		}
		return result;
	}

	void __declspec(naked) AkaiMovieSeekingGuard()
	{
		__asm
		{
			mov eax, dword ptr [esi + 68h]
			test eax, eax
			jz missingInterface
			mov ecx, dword ptr [eax]
			jmp dword ptr [akaiMovieSeekingGuardContinue]
		missingInterface:
			jmp dword ptr [akaiMovieSeekingGuardReturn]
		}
	}

	BOOL CALLBACK FindCurrentProcessWindow(HWND window, LPARAM parameter)
	{
		DWORD processId = 0;
		GetWindowThreadProcessId(window, &processId);
		if (processId != GetCurrentProcessId() ||
			GetWindow(window, GW_OWNER) != nullptr)
			return TRUE;

		*reinterpret_cast<HWND*>(parameter) = window;
		return FALSE;
	}

	DWORD WINAPI WakeAkaiKatanaWindow(LPVOID)
	{
		HWND gameWindow = nullptr;
		for (int attempt = 0; attempt < 200 && gameWindow == nullptr; ++attempt)
		{
			EnumWindows(FindCurrentProcessWindow,
				reinterpret_cast<LPARAM>(&gameWindow));
			if (gameWindow == nullptr)
				Sleep(25);
		}

		if (gameWindow != nullptr)
		{
			// Wine's X11 driver can create Akai Katana's window without
			// queuing the initial paint that its blocking GetMessage loop
			// expects. Wait until D3D9 has finished creating the device, then
			// wake that loop exactly once. Repeated paints are unnecessary and
			// can drive the game's NESYS polling path continuously.
			Sleep(750);
			PostMessageA(gameWindow, WM_PAINT, 0, 0);
		}
		return 0;
	}
#endif

}

static InitFunction initFunction([]()
{
#if _M_IX86
	if (IsEnEinsDiagnosticsEnabled())
	{
		char workingDirectory[MAX_PATH] = {};
		GetCurrentDirectoryA(_countof(workingDirectory), workingDirectory);
		WritePrivateProfileStringA(
			"Launch",
			"CommandLine",
			GetCommandLineA(),
			".\\EnEinsDiagnostic.ini");
		WritePrivateProfileStringA(
			"Launch",
			"WorkingDirectory",
			workingDirectory,
			".\\EnEinsDiagnostic.ini");
		enEinsCreateFileAOri =
			iatHook("kernel32.dll", EnEinsCreateFileAHook, "CreateFileA");
		enEinsCreateFileWOri =
			iatHook("kernel32.dll", EnEinsCreateFileWHook, "CreateFileW");
		enEinsOutputDebugStringAOri =
			iatHook(
				"kernel32.dll",
				EnEinsOutputDebugStringAHook,
				"OutputDebugStringA");
		enEinsMessageBoxAOri =
			iatHook("user32.dll", EnEinsMessageBoxAHook, "MessageBoxA");
		enEinsDirect3DCreate9Ori =
			iatHook(
				"d3d9.dll",
				EnEinsDirect3DCreate9Hook,
				"Direct3DCreate9");
	}
#endif
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if(GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
	if (GameDetect::IsAkaiKatana() && IsWineCompatEnabled())
	{
		const uintptr_t imageBase =
			reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
		akaiMovieSeekingGuardContinue = imageBase + 0x3329F;
		akaiMovieSeekingGuardReturn = imageBase + 0x332D6;

		// Akai Katana's movie update path checks its state flag at +0x14,
		// then unconditionally dereferences the IMediaSeeking-style interface
		// stored at +0x68. Wine can leave that interface null even though the
		// graph reached the active state, crashing at Game.exe+0x3329D when
		// attract enters its first movie. Preserve the original path whenever
		// the interface exists and otherwise use the game's own zero-result
		// return path for that update.
		injector::MakeJMP(
			imageBase + 0x3329A,
			AkaiMovieSeekingGuard,
			true);

		// Wine's ASF source exposes an "Output" pin. Akai interprets that
		// successful lookup as a prebuilt graph and skips SampleGrabber plus
		// RenderFile, leaving its playback interfaces null. Force the game's
		// existing graph-build path; the original videos remain decoded and
		// rendered normally.
		injector::MakeJMP(
			imageBase + 0x33465,
			imageBase + 0x3346D,
			true);

		HANDLE wakeThread = CreateThread(
			nullptr, 0, WakeAkaiKatanaWindow, nullptr, 0, nullptr);
		if (wakeThread != nullptr)
			CloseHandle(wakeThread);
	}
#endif
}, GameID::Nesica);

static int ReturnTrue()
{
	return 1;
}

static InitFunction initFunction_USF4([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::UltraStreetFighterIVDevExe);

static InitFunction initFunction_GC2([]()
{
	uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);
#if _M_IX86
	grooveGetAdaptersInfoOri = iatHook(
		"iphlpapi.dll",
		GrooveGetAdaptersInfo,
		"GetAdaptersInfo");
#endif
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	init_NesysEmu();

	//// Patch D: references
	////D:
	//injector::WriteMemoryRaw(imageBase + 0x33D344, ".", 2, true);
	//// D:/garbage%d.txt
	//injector::WriteMemoryRaw(imageBase + 0x2B6B08, "./garbage%d.txt", 16, true);
	//// D:/country.dat
	//injector::WriteMemoryRaw(imageBase + 0x2B4B68, "./country.dat", 14, true);
	//injector::WriteMemoryRaw(imageBase + 0x2B4B54, "./country.dat", 14, true);
	//// D:/NesysQueue_Error_%04d_%02d_%02d_%02d_%02d_%02d.txt
	//injector::WriteMemoryRaw(imageBase + 0x2B205C, "./NesysQueue_Error_%04d_%02d_%02d_%02d_%02d_%02d.txt", 53, true);
	//// D:/count.csv
	//injector::WriteMemoryRaw(imageBase + 0x2B1024, "./count.csv", 12, true);
	//injector::WriteMemoryRaw(imageBase + 0x2B0F40, "./count.csv", 12, true);
	//// D:\\%s/
	//injector::WriteMemoryRaw(imageBase + 0x27AD80, "./%s/", 6, true);
	//// D:\\%s/*
	//injector::WriteMemoryRaw(imageBase + 0x27AD78, "./%s/*", 7, true);
	//// "D:\\%s/%s"
	//injector::WriteMemoryRaw(imageBase + 0x27AD6C, "./%s/%s", 8, true);
	//// "D:\\"
	//injector::WriteMemoryRaw(imageBase + 0x27AC44, "./", 3, true);
	//// D:\\%s%04d%02d%02d_%02d%02d%02d_
	//injector::WriteMemoryRaw(imageBase + 0x27AC00, "./%s%04d%02d%02d_%02d%02d%02d_", 31, true);
	//// D:\\%s/%s/*
	//injector::WriteMemoryRaw(imageBase + 0x27AD60, "./%s/%s/*", 10, true);
	//// D:/PlayData/
	//injector::WriteMemoryRaw(imageBase + 0x2A9CB8, "./PlayData/", 12, true);

	// C:\\TypeXZEROTemp.dat check
	safeJMP(imageBase + 0xF81B0, ReturnTrue);

	// Ignore stupid OutputDebugStringA Scheluder crap
	injector::MakeNOP(imageBase + 0x2008CC, 7);

	// Unstuck the game from some dumb mouse scanner func
	injector::MakeNOP(imageBase + 0xA3FF6, 2);

	// Patch dongle spam on RFID port
	injector::MakeNOP(imageBase + 0xF90F6, 5);
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::GrooveCoaster2);

static InitFunction initFunction_DariusBurst([]()
{
	init_FastIoEmu();
	init_RegHooks();
	init_NesysEmu(true);

	auto imageBase = (uintptr_t)GetModuleHandleA(0);

	// Ignore cryptopipe check.
	// NOTE: This could be cause for the non-working TEST MODE. No time to analyze since dump was released and we want to give instant support.
	// Update 2025:
	// Something in modern windows 11 breaks CryptDecrypt. For now, i'll patch the check
	// and also patch the second check so that the test menu also works. I did not notice any issues with this, so
	// hopefully this will be fine for now until we can figure out if we can fix windows 11 compatibility.
	injector::WriteMemory<BYTE>(imageBase + 0x2CC753, 0xEB, true);
	injector::WriteMemory<BYTE>(imageBase + 0x1de917, 0x75, true);
	
	// D:
	injector::WriteMemoryRaw(imageBase + 0x482F38, "\x2E\x5C\x44", 3, true); // D:\%s%04d%02d%02d_%02d%02d%02d_
	injector::WriteMemoryRaw(imageBase + 0x4830A0, "\x2E\x5C\x44", 3, true); // D:\%s/%s/*
	injector::WriteMemoryRaw(imageBase + 0x4830AC, "\x2E\x5C\x44", 3, true); // D:\%s/%s
	injector::WriteMemoryRaw(imageBase + 0x4830B3, "\x2E\x5C\x44", 3, true); // D:\%s/*
	injector::WriteMemoryRaw(imageBase + 0x49FC90, "\x2E\x5C\x44", 3, true); // D:\%s
	injector::WriteMemoryRaw(imageBase + 0x4A269C, "\x2E\x5C\x44", 3, true); // EDData
	injector::WriteMemoryRaw(imageBase + 0x4AA168, "\x2E\x5C\x44", 3, true);
	injector::WriteMemoryRaw(imageBase + 0x4D460C, "\x2E\x5C\x44", 3, true);
	injector::WriteMemoryRaw(imageBase + 0x4D46A8, "\x2E\x5C\x44", 3, true); // Proclog

	// D:/
	injector::WriteMemoryRaw(imageBase + 0x4D44B4, "\x2E\x5C\x44", 3, true);

	// Disable invertion of 2nd screen area
	// NOTE: Nezarn is pro
	injector::WriteMemoryRaw(imageBase + 0x4D4E34, "\x30\x2E\x30\x66\x20\x20\x20\x20", 8, true); // 0.0f
	injector::WriteMemoryRaw(imageBase + 0x4D4E4C, "\x2A\x20\x30\x2E\x30\x66\x20\x20\x20\x20\x20\x2D", 12, true); // * 0.0f -
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::DariusBurst);

static InitFunction initFunction_DariusBurst116([]()
{
	init_FastIoEmu();
	init_RegHooks();
	init_NesysEmu(true);

	auto imageBase = (uintptr_t)GetModuleHandleA(nullptr);

	// Ignore cryptopipe check.
	// NOTE: This could be cause for the non-working TEST MODE. No time to analyze since dump was released and we want to give instant support.
	// Update 2025:
	// Something in modern windows 11 breaks CryptDecrypt. For now, i'll patch the check
	// and also patch the second check so that the test menu also works. I did not notice any issues with this, so
	// hopefully this will be fine for now until we can figure out if we can fix windows 11 compatibility.
	injector::WriteMemory<BYTE>(imageBase + 0x302743, 0xEB, true);
	injector::WriteMemory<BYTE>(imageBase + 0x1fb867, 0x75, true);

	//// D:
	injector::WriteMemoryRaw(imageBase + 0x4EEF68, "\x2E\x5C\x44", 3, true); // D:\%s%04d%02d%02d_%02d%02d%02d_
	injector::WriteMemoryRaw(imageBase + 0x4EF0D0, "\x2E\x5C\x44", 3, true); // D:\%s/%s/*
	injector::WriteMemoryRaw(imageBase + 0x4EF0DC, "\x2E\x5C\x44", 3, true); // D:\%s/%s
	injector::WriteMemoryRaw(imageBase + 0x4EF0E8, "\x2E\x5C\x44", 3, true); // D:\%s/*
	injector::WriteMemoryRaw(imageBase + 0x4EF0F0, "\x2E\x5C\x44", 3, true); // D:\%s
	injector::WriteMemoryRaw(imageBase + 0x50E980, "\x2E\x5C\x44", 3, true); // D:\EDData
	injector::WriteMemoryRaw(imageBase + 0x50EB58, "\x2E\x5C\x44", 3, true); // D:\EDData
	injector::WriteMemoryRaw(imageBase + 0x5145E0, "\x2E\x5C\x44", 3, true); // D:\EDData
	injector::WriteMemoryRaw(imageBase + 0x539190, "\x2E\x5C\x44", 3, true); // D:\EDData
	injector::WriteMemoryRaw(imageBase + 0x539240, "\x2E\x5C\x44", 3, true); // Proclog
	injector::WriteMemoryRaw(imageBase + 0x50DD84, "\x2E\x5C\x44", 3, true); // D:\EDData\event000.pxk
	injector::WriteMemoryRaw(imageBase + 0x50DD9C, "\x2E\x5C\x44", 3, true); // D:\EDData\ev
	injector::WriteMemoryRaw(imageBase + 0x50E8EC, "\x2E\x5C\x44", 3, true); // D:\EDData\ev
	injector::WriteMemoryRaw(imageBase + 0x50E8FC, "\x2E\x5C\x44", 3, true); // D:\EDData\ev\event000.sxr
	injector::WriteMemoryRaw(imageBase + 0x50EB64, "\x2E\x5C\x44", 3, true); // D:\EDData\ev\event000.sxr
	injector::WriteMemoryRaw(imageBase + 0x50EBE8, "\x2E\x5C\x44", 3, true); // D:\EDData\ev
	injector::WriteMemoryRaw(imageBase + 0x50EBF8, "\x2E\x5C\x44", 3, true); // D:\EDData\ev\event000.sxr
	injector::WriteMemoryRaw(imageBase + 0x517464, "\x2E\x5C\x44", 3, true); // D:\EDData\news000.tx2
	injector::WriteMemoryRaw(imageBase + 0x51747C, "\x2E\x5C\x44", 3, true); // D:\EDData\news000.tx2
	injector::WriteMemoryRaw(imageBase + 0x517494, "\x2E\x5C\x44", 3, true); // D:\EDData\news000.tx2
	injector::WriteMemoryRaw(imageBase + 0x518524, "\x2E\x5C\x44", 3, true); // D:\EDData\ev
	injector::WriteMemoryRaw(imageBase + 0x5660FC, "\x2E\x5C\x44", 3, true); // D:\EDData\ev
	injector::WriteMemoryRaw(imageBase + 0x56610C, "\x2E\x5C\x44", 3, true); // D:\EDData\ev\event000.sxr

	// D:/
	injector::WriteMemoryRaw(imageBase + 0x539020, "\x2E\x5C\x44", 3, true);

	// Disable invertion of 2nd screen area
	injector::WriteMemoryRaw(imageBase + 0x5399CC, "\x30\x2E\x30\x66\x20\x20\x20\x20", 8, true); // 0.0f
	injector::WriteMemoryRaw(imageBase + 0x5399E4, "\x2A\x20\x30\x2E\x30\x66\x20\x20\x20\x20\x20\x2D", 12, true); // * 0.0f -
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::DariusBurst116);

static InitFunction initFunction_PB([]()
{
	uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	init_NesysEmu();

	// Nesys error workaround
	injector::WriteMemoryRaw(imageBase + 0xA77B, "\xA3\xEC\x0D\x4F\x00\x90", 6, true);
}, GameID::PuzzleBobble);

static InitFunction initFunction_MB([]()
{
	uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
	// Skip Initilization wait time.
	injector::MakeNOP(imageBase + 0x56B21, 2);
}, GameID::MagicalBeat);

static InitFunction initFunction_CC([]()
{
	uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
	// Skip stuck on Warning screen. NESYS emu must be improved for this to work properly!
	injector::MakeNOP(imageBase + 0x1015E7, 2);
}, GameID::CrimzonClover);

static InitFunction initFunction_SOR([]()
{
	uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);
	init_FastIoEmu();
	init_RfidEmu();
	// TODO: DOCUMENT PATCHES
	safeJMP(imageBase + 0xFA350, ReturnTrue);
	safeJMP(imageBase + 0xF8FC0, ReturnTrue);
	// Patch data dir to game dir pls D:/ -> .\\
	//
	injector::WriteMemory<DWORD>(imageBase + 0x21B9AC0, 0x2F002E002E, true);
	init_NesysEmu();
	if (ToBool(config["General"]["Windowed"]))
	{
		// TODO: DOCUMENT PATCHES
		injector::WriteMemory<LONGLONG>(imageBase + 0xFF703C, 0xF633C1FFC1FFC933, true);
		injector::WriteMemory<DWORD>(imageBase + 0xFF703C+0x08, 0xC6FFC6FF, true);
	}
}, GameID::SchoolOfRagnarok);

static InitFunction initFunction_HyperSF4([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	//init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::HyperStreetFighterII);

static InitFunction initFunction_StreetFighterZero3([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	//init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::StreetFigherZero3);

static InitFunction initFunction_StreetFighter3rdStrike([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	//init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::StreetFighter3rdStrike);

static InitFunction initFunction_RumbleFish2([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::RumbleFish2);

static InitFunction initFunction_KOF98Nesica([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::KOF98Nesica);

static InitFunction initFunction_VampireSavior([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::VampireSavior);

static InitFunction initFunction_ChaosCode([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::ChaosCode);

static InitFunction initFunction_DoNotFall([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
	CreateDirectoryA(".\\OpenParrot\\system", nullptr); // needed for test menu saving lol
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::DoNotFall);

static InitFunction initFunction_HomuraNesica([]()
{
	uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
	if (ToBool(config["General"]["Windowed"]))
	{
		// don't set cursor POS to X=240 Y=320
		injector::MakeNOP(imageBase + 0x123394, 16, true);
		injector::MakeNOP(imageBase + 0x1235AE, 16, true);

		// show cursor
		injector::WriteMemory<BYTE>(imageBase + 0x1233A5, 0x01, true);
		injector::WriteMemory<BYTE>(imageBase + 0x1235BF, 0x01, true);
	}

}, GameID::HomuraNesica);

static InitFunction initFunction_RaidenIVNesica([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::RaidenIVNesica);

static InitFunction initFunction_SenkoNoRondeDuoNesica([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::SenkoNoRondeDuoNesica);

static InitFunction initFunction_SkullGirls([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::SkullGirls);

static InitFunction initFunction_TroubleWitchesNesica([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::TroubleWitchesNesica);

static InitFunction initFunction_Yatagarasu([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::Yatagarasu);

static InitFunction initFunction_Exception([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::Exception);

static InitFunction initFunction_KOF2002([]()
{
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif
}, GameID::KOF2002);

static InitFunction initFunction_BlazBlueCF201([]()
{
	uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif

	// skip nesys error
	injector::MakeNOP(imageBase + 0x7D932, 6, true);
	injector::MakeJMP(imageBase + 0x7D932, imageBase + 0x7DA03, true);

	// unlock colors
	injector::WriteMemory<BYTE>(imageBase + 0x1B0408, 0x18, true);
	injector::WriteMemory<BYTE>(imageBase + 0x1B04D2, 0x18, true);
	injector::WriteMemory<BYTE>(imageBase + 0x1B04FD, 0x75, true);

}, GameID::BlazBlueCF201);

static InitFunction initFunction_DarkAwake([]()
{
	uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);
	DWORD oldPageProtection = 0;
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif

	if (ToBool(config["General"]["Windowed"])) 
	{
		// force windowed mode
		injector::MakeNOP(imageBase + 0x169BF, 2, true);

		VirtualProtect((LPVOID)(imageBase + 0x601CC), 64, PAGE_EXECUTE_READWRITE, &oldPageProtection);
		windowHooks hooks = { 0 };
		hooks.createWindowExA = imageBase + 0x601E8;
		hooks.setWindowPos = imageBase + 0x601F0;
		init_windowHooks(&hooks);
		VirtualProtect((LPVOID)(imageBase + 0x601CC), 64, oldPageProtection, &oldPageProtection);

		// change window name
		static const char* title = "OpenParrot - Dark Awake";
		injector::WriteMemory<DWORD>(imageBase + 0x16736, (DWORD)title, true);

		// don't resize to current work area
		injector::MakeNOP(imageBase + 0x377F2, 15, true);

		// show cursor
		injector::WriteMemory<BYTE>(imageBase + 0x165D9, 0x01, true);
	}

}, GameID::DarkAwake);

static InitFunction initFunction_ChaosBreakerNXL([]()
{
	uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);
	DWORD oldPageProtection = 0;
	init_FastIoEmu();
	init_RfidEmu();
	init_RegHooks();
	if (GameDetect::enableNesysEmu)
		init_NesysEmu();
#if _M_IX86
	init_CryptoPipe(GameDetect::NesicaKey);
#endif

	if (ToBool(config["General"]["Windowed"]))
	{
		// force windowed mode
		injector::MakeNOP(imageBase + 0x169AF, 2, true);

		VirtualProtect((LPVOID)(imageBase + 0x601CC), 64, PAGE_EXECUTE_READWRITE, &oldPageProtection);
		windowHooks hooks = { 0 };
		hooks.createWindowExA = imageBase + 0x601E8;
		hooks.setWindowPos = imageBase + 0x601F0;
		init_windowHooks(&hooks);
		VirtualProtect((LPVOID)(imageBase + 0x601CC), 64, oldPageProtection, &oldPageProtection);

		// change window name
		static const char* title = "OpenParrot - Chaos Breaker";
		injector::WriteMemory<DWORD>(imageBase + 0x16726, (DWORD)title, true);

		// don't resize to current work area
		injector::MakeNOP(imageBase + 0x37452, 15, true);

		// show cursor
		injector::WriteMemory<BYTE>(imageBase + 0x165C9, 0x01, true);
	}

}, GameID::ChaosBreakerNXL);

static InitFunction initFunction_Theatrhythm([]()
	{
		uintptr_t imageBase = (uintptr_t)GetModuleHandleA(0);

		static std::string modPath;

		if (modPath.empty())
		{
			char exeName[512];
			GetModuleFileNameA(GetModuleHandle(L"OpenParrot64.dll"), exeName, sizeof(exeName));

			char* exeBaseName = strrchr(exeName, '\\');
			exeBaseName[0] = L'\0';

			modPath = exeName;
			modPath += "\\";

			GetFullPathNameA(modPath.c_str(), sizeof(exeName), exeName, nullptr);

			modPath = exeName;
			modPath += "\\";
		}

		std::string idmac = modPath + "idmacdrv64.dll";

		uintptr_t idmacbase = (uintptr_t)LoadLibraryA(idmac.c_str());

		if (idmacbase == NULL)
			ExitProcess(0);

		init_FastIoEmu();
	
	}, GameID::Theatrhythm);

#if _M_IX86
static InitFunction initFunction_KOFXIIIClimax([]()
{
	init_CryptoPipe(GameDetect::NesicaKey);
}, GameID::KOFXIIIClimax);

static InitFunction initFunction_KOFXIII([]()
{
		init_CryptoPipe(GameDetect::NesicaKey);
}, GameID::KOFXIII);
#endif

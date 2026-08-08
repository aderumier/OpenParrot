#include <StdInc.h>
#include "InitFunction.h"

#include <forward_list>

std::forward_list<InitFunction*>* g_initFunctions;

#if _M_IX86
static bool IsGhaInitDiagnosticsEnabled()
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

static void TraceGhaInitFunctions(
	GameID requestedGame,
	int registeredGame,
	bool invoked)
{
	if (!IsGhaInitDiagnosticsEnabled())
		return;
	if (requestedGame != GameID::GHA)
		return;

	HMODULE module = nullptr;
	if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&TraceGhaInitFunctions),
			&module))
	{
		return;
	}

	char modulePath[MAX_PATH] = {};
	if (GetModuleFileNameA(
			module, modulePath, static_cast<DWORD>(std::size(modulePath))) == 0)
	{
		return;
	}
	char* separator = strrchr(modulePath, '\\');
	if (separator == nullptr)
		return;
	strcpy_s(
		separator + 1,
		MAX_PATH - static_cast<size_t>(separator + 1 - modulePath),
		"OpenParrotGHAInitFunctions.log");

	HANDLE file = CreateFileA(
		modulePath,
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;

	char line[128] = {};
	const int length = sprintf_s(
		line,
		"request=%d registered=%d invoked=%d\r\n",
		static_cast<int>(requestedGame),
		registeredGame,
		invoked ? 1 : 0);
	DWORD written = 0;
	WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
	CloseHandle(file);
}
#endif

InitFunction::InitFunction(void(*callback)(), GameID gameID)
	: callback(callback), game(gameID)
{
	if (!g_initFunctions)
	{
		g_initFunctions = new std::forward_list<InitFunction*>();
	}

	g_initFunctions->push_front(this);
}

void InitFunction::RunFunctions(GameID game)
{
#if _M_IX86
	TraceGhaInitFunctions(game, -1, false);
#endif
	for (auto& it : *g_initFunctions)
	{
#if _M_IX86
		TraceGhaInitFunctions(
			game,
			static_cast<int>(it->game),
			it->game == game);
#endif
		if (it->game == game)
		{
			it->callback();
		}
	}
}

#include "StdInc.h"
#include "WineCompat.h"
#include <string.h>

namespace
{
	enum class Tristate
	{
		Unknown,
		False,
		True
	};

	Tristate DetectWineHost()
	{
		const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		return ntdll != nullptr &&
			GetProcAddress(ntdll, "wine_get_version") != nullptr
			? Tristate::True
			: Tristate::False;
	}

	// TP_FORCE_WINE=0/false/off keeps the native path under Wine; any other
	// value enables the Wine paths, which also allows exercising them on
	// native Windows.
	Tristate ReadForceOverride()
	{
		char value[16] = {};
		const DWORD length = GetEnvironmentVariableA(
			"TP_FORCE_WINE",
			value,
			_countof(value));
		if (length == 0 || length >= _countof(value))
			return Tristate::Unknown;

		if (_stricmp(value, "0") == 0 ||
			_stricmp(value, "false") == 0 ||
			_stricmp(value, "off") == 0 ||
			_stricmp(value, "disabled") == 0)
			return Tristate::False;

		return Tristate::True;
	}
}

bool IsRunningUnderWine()
{
	static const Tristate underWine = DetectWineHost();
	return underWine == Tristate::True;
}

bool IsAndroidWineRuntime()
{
	static const bool androidRuntime =
		GetEnvironmentVariableA("ANDROID_ALSA_SERVER", nullptr, 0) != 0;
	return androidRuntime;
}

bool IsWineCompatEnabled()
{
	static const Tristate forced = ReadForceOverride();
	if (forced != Tristate::Unknown)
		return forced == Tristate::True;

	// This branch targets Wine exclusively, so the compatibility paths are on by
	// default instead of being gated on host detection. Wine Staging can be
	// built or configured to hide ntdll's wine_* exports, and falling back to
	// the native Windows paths there would be a silent loss of every fix below.
	return true;
}

#pragma once

#include <windows.h>
#include <string>

size_t GetByteSizeOfWchar(const wchar_t* str);
wchar_t* GetLastErrorAsString();
wchar_t* GetFileVersion(const wchar_t* pszFilePath);

// True when the loader is hosted by Wine, whatever the flavour: desktop
// Linux/macOS Wine, Proton, or the Android Winlator runtime. Detected through
// ntdll's wine_get_version export and cached after the first call.
bool IsRunningUnderWine();
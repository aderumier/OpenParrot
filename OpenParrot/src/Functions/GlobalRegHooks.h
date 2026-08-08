#pragma once
void init_GlobalRegHooks();
LSTATUS __stdcall RegOpenKeyExWGlobalWrap(
	HKEY hKey,
	LPCWSTR lpSubKey,
	DWORD ulOptions,
	REGSAM samDesired,
	PHKEY phkResult);
LSTATUS __stdcall RegCreateKeyExWGlobalWrap(
	HKEY hKey,
	LPCWSTR lpSubKey,
	DWORD Reserved,
	LPWSTR lpClass,
	DWORD dwOptions,
	REGSAM samDesired,
	CONST LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	PHKEY phkResult,
	LPDWORD lpdwDisposition);
LSTATUS __stdcall RegQueryValueExWGlobalWrap(
	HKEY hKey,
	LPCWSTR lpValueName,
	LPDWORD lpReserved,
	LPDWORD lpType,
	__out_data_source(REGISTRY) LPBYTE lpData,
	LPDWORD lpcbData);

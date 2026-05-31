workspace "OpenParrot"
	configurations { "Debug", "Release"}
	platforms { "x64", "x86" }

	flags { "No64BitChecks" }

	staticruntime "On"
	editandcontinue "Off"

	systemversion "latest"

	symbols "On"

	characterset "Unicode"

	flags { "NoIncrementalLink", "NoMinimalRebuild" }

	includedirs { "deps/inc/", "deps/udis86/" }

	libdirs { "deps/lib/" }

	buildoptions { "/MP", "/std:c++17" }

	configuration "Debug*"
		targetdir "build/bin/debug"
		defines "NDEBUG"
		objdir "build/obj/debug"

	configuration "Release*"
		targetdir "build/bin/release"
		defines "NDEBUG"
		optimize "speed"
		objdir "build/obj/release"

	filter "platforms:x86"
		architecture "x32"

	filter "platforms:x64"
		architecture "x64"

project "MinHook"
	targetname "MinHook"
	language "C"
	kind "StaticLib"

	files
	{
		"deps/src/buffer.c", "deps/src/hook.c",
		"deps/src/trampoline.c",
	}

	filter "platforms:x86"
		files { "deps/src/hde/hde32.c" }

	filter "platforms:x64"
		files { "deps/src/hde/hde64.c" }

project "udis86"
	targetname "udis86"
	language "C"
	kind "StaticLib"

	includedirs
	{
		"deps/udis86/"
	}

	files
	{
		"deps/udis86/libudis86/*.c"
	}

include "OpenParrot"
include "OpenParrotLoader"
include "OpenParrotKonamiLoader"
include "iDmacDrv"
include "OpenBanapass"

-- Standalone door overlay for Elevator Action Death Parade (x86 only)
project "EADPDoorOverlay"
	targetname "EADPDoorOverlay"
	language "C++"
	kind "WindowedApp"

	files   { "EADPLoader/EADPDoorOverlay.cpp" }
	links   { "gdiplus", "shlwapi", "user32", "gdi32", "kernel32" }

	filter "platforms:x86"
		-- Only x86: the game is 32-bit; we read its stack via ReadProcessMemory
		-- and hardware breakpoints require matching bitness.

	filter "platforms:x64"
		flags { "ExcludeFromBuild" }
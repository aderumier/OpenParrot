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

-- EADP door overlay (Wine-compatible, x86 only — game is 32-bit)
project "EADPDoorHook"
	targetname "EADPDoorHook"
	language "C++"
	kind "SharedLib"

	filter "platforms:x86"
		files
		{
			"EADPLoader/EADPDoorHook.cpp",
			"deps/src/buffer.c",
			"deps/src/hook.c",
			"deps/src/trampoline.c",
			"deps/src/hde/hde32.c",
		}
		includedirs { "EADPLoader", "deps/inc/", "deps/src/" }
		links { }

	filter "platforms:x64"
		-- Hook DLL must be 32-bit (game is 32-bit); skip x64 build
		files { }

project "EADPDoorOverlay"
	targetname "EADPDoorOverlay"
	language "C++"
	kind "WindowedApp"

	files { "EADPLoader/EADPDoorOverlay.cpp" }
	includedirs { "EADPLoader" }
	links { "gdiplus", "shlwapi", "user32", "gdi32", "kernel32" }

	filter "platforms:x86"
		-- nothing extra
	filter "platforms:x64"
		-- nothing extra
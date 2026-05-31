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

-- EADP door overlay – DLL renders via D3D9 EndScene hook, x86 only
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
		libdirs     { "deps/inc/DirectXSDK/Lib/x86" }
		links       { "d3d9", "d3dx9", "shlwapi" }

	filter "platforms:x64"
		files { }   -- game is 32-bit; skip x64

project "EADPDoorOverlay"
	targetname "EADPDoorOverlay"
	language "C++"
	kind "WindowedApp"

	-- Pure injector: no GDI, no D3D9 – all rendering is in EADPDoorHook.dll
	files { "EADPLoader/EADPDoorOverlay.cpp" }
	links { "shlwapi", "user32", "kernel32" }

	filter "platforms:x86"
	filter "platforms:x64"
		flags { "ExcludeFromBuild" }  -- inject into 32-bit game; x86 exe only
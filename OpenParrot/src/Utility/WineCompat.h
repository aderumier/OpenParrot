#pragma once

// Wine host detection shared by the per-game compatibility paths.
//
// Historically each Wine workaround was gated on ANDROID_ALSA_SERVER, because
// the Winlator/Box64 runtime was the only place they had been exercised. Most
// of those workarounds do not fix an Android or Box64 problem at all: they fix
// a Wine one (stubbed iphlpapi/dpnet/WinSCard/wbemprox entry points, quartz
// interfaces Wine leaves null, D3D8 texture limits, windows Winex11 has not
// mapped yet). Those apply to every Wine host, including plain x86 desktop
// Wine and Proton, so they are gated on IsWineCompatEnabled() instead.
//
// Workarounds that really are Android/Box64 specific - x87 emulation traps,
// touch input, the Winlator network namespace, high-refresh phone panels -
// stay on IsAndroidWineRuntime() and are unchanged on desktop Wine.

// True when the process is hosted by Wine, whatever the flavour: desktop
// Linux/macOS Wine, Proton, or the Android Winlator runtime. Detected through
// ntdll's wine_get_version export and cached after the first call.
bool IsRunningUnderWine();

// True when the Android (Winlator) Wine runtime is hosting the process, i.e.
// when its ALSA bridge variable has been injected into the environment.
bool IsAndroidWineRuntime();

// Gate for compatibility work that addresses a Wine limitation rather than an
// Android/Box64 one.
//
// This branch targets Wine exclusively, so it is true by default rather than
// conditional on IsRunningUnderWine(): a prefix that hides ntdll's wine_*
// exports must not silently fall back to the native Windows paths.
//
// TP_FORCE_WINE=0/false/off turns the whole group off again, which is the way
// to check whether one of these paths is responsible for a regression.
bool IsWineCompatEnabled();

// HostSession.h
//
// "Bu bilgisayarı yayınla": one hosting session of the unified app --
// the former server.exe main loop as a callable. Wires ScreenCapture ->
// (VideoScaler) -> NvencEncoder -> StreamSender, plus AudioCapture,
// InputInjector, the LAN DiscoveryResponder and the StatusWindow that
// shows the pairing code.
//
// Returns when the status window is closed (the session's "stop button"),
// or `interrupted` becomes true (Ctrl+C with a console attached), or a
// fatal startup error occurs. All modules are torn down before returning,
// so the caller can go straight back to the launcher -- or host again.
//
// Blocking call; run it on the main thread. Return value: 0 = ended
// normally, 1 = failed to start (details on stderr).

#pragma once

#include "AppConfig.h"

#include <atomic>

int RunHostSession(const AppConfig& config, const std::atomic<bool>& interrupted);

// ClientSession.h
//
// "Bağlan": one viewing session of the unified app -- the former
// client.exe session as a callable. Wires StreamReceiver -> VideoDecoder
// -> RenderWindow (fullscreen), AudioPlayer, and InputCapture with the
// wheel / absolute-pointer hooks.
//
// Returns when the user ends the session (Shift+ESC or closing the
// render window), `interrupted` becomes true, or a fatal startup error
// occurs. All modules are torn down before returning, so the caller can
// go straight back to the launcher.
//
// Blocking call; run it on the main thread (the render window's message
// queue lives there). Requires COM initialized on that thread
// (CoInitializeEx MULTITHREADED in app_main).

#pragma once

#include "AppConfig.h"

#include <atomic>
#include <cstdint>
#include <string>

void RunClientSession(const AppConfig& config, const std::string& serverIp,
                      uint16_t serverPort, const std::atomic<bool>& interrupted);

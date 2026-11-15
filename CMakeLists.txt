# MidnightSun -- low-latency game streaming (Windows-only).
#
# ONE target: MidnightSun.exe. The same binary hosts a stream ("Bu
# bilgisayarı yayınla") or connects to one; the launcher window picks the
# role per session, so there is no separate server/client executable.
# GUI subsystem (wWinMain) -- set "show_console": 1 in
# midnightsun_config.json to attach a log console.
#
# Third-party SDKs expected under extern/ (not fetched by CMake):
#   extern/nvenc/nvEncodeAPI.h                 -- NVENC headers (runtime DLL load)
#   extern/opus/{include,lib/x64/opus.lib}     -- Opus codec
#   extern/vigemclient/{include,lib/x64/ViGEmClient.lib} -- ViGEm bus client
#
# Usage:
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.20)
project(MidnightSun CXX)

if(NOT WIN32)
    message(FATAL_ERROR "MidnightSun targets Windows only (DXGI, NVENC, WASAPI, Winsock2).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------

add_executable(MidnightSun WIN32
    app_main.cpp
    AppConfig.cpp
    # host side
    HostSession.cpp
    ScreenCapture.cpp
    NvencEncoder.cpp
    VideoScaler.cpp
    StreamSender.cpp
    InputInjector.cpp
    AudioCapture.cpp
    StatusWindow.cpp
    # client side
    ClientSession.cpp
    StreamReceiver.cpp
    InputCapture.cpp
    AudioPlayer.cpp
    VideoDecoder.cpp
    RenderWindow.cpp
    LauncherWindow.cpp
    # shared
    Crypto.cpp
    Discovery.cpp
    LatencyTracker.cpp
    # kaynaklar: ikon + sürüm bilgisi; manifest mt.exe ile bağlayıcı
    # manifestine karışır (CMake .manifest kaynaklarını otomatik işler)
    MidnightSun.rc
    MidnightSun.manifest
)
set_target_properties(MidnightSun PROPERTIES OUTPUT_NAME MidnightSun)

target_include_directories(MidnightSun PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/extern/nvenc
    ${CMAKE_SOURCE_DIR}/extern/opus/include
    ${CMAKE_SOURCE_DIR}/extern/vigemclient/include
)
# Source files carry #pragma comment(lib, "extern/.../*.lib") with paths
# relative to the repo root; give the linker that root as a search dir.
target_link_directories(MidnightSun PRIVATE ${CMAKE_SOURCE_DIR})
if(MSVC)
    # /utf-8: kaynak dosyalar BOM'suz UTF-8 -- bu bayrak olmadan Türkçe
    # geniş literaller (L"Yayınla") sistem kod sayfasıyla yanlış çözülür.
    target_compile_options(MidnightSun PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
# UNICODE builds only: the UI code calls LoadCursorW(..., IDC_ARROW) and
# relies on d2d1.h's DrawText being macro-renamed to DrawTextW, both of
# which require the wide Win32 macro set.
target_compile_definitions(MidnightSun PRIVATE UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# Link libraries (the .cpp files also carry #pragma comment(lib) for MSVC;
# listing them here keeps the dependencies explicit and non-MSVC-proof)
# ---------------------------------------------------------------------------

target_link_libraries(MidnightSun PRIVATE
    ws2_32
    bcrypt
    winmm
    d3d11
    dxgi
    d2d1
    dwrite
    dwmapi
    mf
    mfplat
    mfuuid
    ole32
    oleaut32
    avrt
    setupapi
    xinput
    ${CMAKE_SOURCE_DIR}/extern/opus/lib/x64/opus.lib
    ${CMAKE_SOURCE_DIR}/extern/vigemclient/lib/x64/ViGEmClient.lib
)

# ---------------------------------------------------------------------------
# Config file: copied into the build tree at configure time, and next to
# the executable at build time (multi-config generators put binaries in
# per-config subdirectories).
# ---------------------------------------------------------------------------

configure_file(${CMAKE_SOURCE_DIR}/midnightsun_config.json
               ${CMAKE_BINARY_DIR}/midnightsun_config.json COPYONLY)

add_custom_command(TARGET MidnightSun POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/midnightsun_config.json
            $<TARGET_FILE_DIR:MidnightSun>/midnightsun_config.json)

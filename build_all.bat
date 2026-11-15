@echo off
setlocal
rem ===========================================================================
rem MidnightSun - tek komutla tam derleme
rem
rem 1) extern/opus      -> opus.lib        (bir kez derlenir, sonra atlanir)
rem 2) extern/vigemclient -> ViGEmClient.lib (bir kez derlenir, sonra atlanir)
rem 3) Proje            -> build\Release\MidnightSun.exe (tek uygulama:
rem                        yayinci + baglanan taraf ayni exe)
rem
rem Gereksinim: CMake (PATH'te) + Visual Studio 2022 (C++ workload).
rem Yeniden tam derleme icin: build_all.bat clean
rem ===========================================================================

cd /d "%~dp0"

rem --- CMake'i bul: once PATH, yoksa vswhere ile VS kurulumundaki CMake -----
rem (Dikkat: "Program Files (x86)" icindeki parantez yuzunden bu bolumde
rem  parantezli blok kullanilmiyor.)
where cmake >nul 2>nul
if not errorlevel 1 goto :cmake_ok

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :cmake_missing

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :cmake_missing
if not exist "%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" goto :cmake_missing

set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
echo [BILGI] CMake VS kurulumunda bulundu: "%VSPATH%"

where cmake >nul 2>nul
if not errorlevel 1 goto :cmake_ok

:cmake_missing
echo [HATA] cmake bulunamadi. Visual Studio Installer'dan "C++ CMake tools
echo        for Windows" bilesenini kurun veya cmake.org'dan yukleyin,
echo        ya da bu betigi "Developer Command Prompt for VS 2022" icinde calistirin.
exit /b 1

:cmake_ok

if /i "%~1"=="clean" (
    echo [TEMIZLIK] build klasorleri siliniyor...
    rmdir /s /q build 2>nul
    rmdir /s /q extern\opus\build 2>nul
    rmdir /s /q extern\vigemclient\build 2>nul
    del /q extern\opus\lib\x64\opus.lib 2>nul
    del /q extern\vigemclient\lib\x64\ViGEmClient.lib 2>nul
)

rem --- 1. Opus ---------------------------------------------------------------
if exist extern\opus\lib\x64\opus.lib (
    echo [ATLA] opus.lib zaten mevcut.
) else (
    echo [1/3] Opus derleniyor...
    cmake -S extern\opus -B extern\opus\build -G "Visual Studio 17 2022" -A x64 ^
        -DOPUS_BUILD_SHARED_LIBRARY=OFF ^
        -DOPUS_BUILD_TESTING=OFF ^
        -DOPUS_BUILD_PROGRAMS=OFF
    if errorlevel 1 goto :fail
    cmake --build extern\opus\build --config Release --parallel
    if errorlevel 1 goto :fail
    if not exist extern\opus\lib\x64 mkdir extern\opus\lib\x64
    copy /y extern\opus\build\Release\opus.lib extern\opus\lib\x64\opus.lib >nul
    if errorlevel 1 goto :fail
    echo [OK] extern\opus\lib\x64\opus.lib
)

rem --- 2. ViGEmClient ---------------------------------------------------------
if exist extern\vigemclient\lib\x64\ViGEmClient.lib (
    echo [ATLA] ViGEmClient.lib zaten mevcut.
) else (
    echo [2/3] ViGEmClient derleniyor...
    cmake -S extern\vigemclient -B extern\vigemclient\build -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 goto :fail
    cmake --build extern\vigemclient\build --config Release --parallel
    if errorlevel 1 goto :fail
    if not exist extern\vigemclient\lib\x64 mkdir extern\vigemclient\lib\x64
    copy /y extern\vigemclient\build\Release\ViGEmClient.lib extern\vigemclient\lib\x64\ViGEmClient.lib >nul
    if errorlevel 1 goto :fail
    echo [OK] extern\vigemclient\lib\x64\ViGEmClient.lib
)

rem --- 3. MidnightSun ----------------------------------------------------------
echo [3/3] MidnightSun derleniyor...
cmake -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :fail
cmake --build build --config Release --parallel
if errorlevel 1 goto :fail

echo.
echo =========================================================
echo  BASARILI
echo    Uygulama : build\Release\MidnightSun.exe
echo               (+ midnightsun_config.json)
echo.
echo  Ayni exe hem yayinci hem baglanan taraf: acilis
echo  ekranindan "Bu bilgisayari yayinla" ya da listeden/
echo  kodla baglanmayi secin.
echo.
echo  Not: Gamepad enjeksiyonu icin yayinci PC'de ViGEmBus
echo  surucusu kurulu olmali (yoksa klavye/mouse ile devam eder):
echo    https://github.com/nefarius/ViGEmBus/releases
echo =========================================================
exit /b 0

:fail
echo.
echo [HATA] Derleme basarisiz oldu. Yukaridaki ciktiya bakin.
exit /b 1

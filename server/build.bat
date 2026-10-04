@echo off
rem Build the relay server. Standalone - it shares only the wire format with
rem MAME, by compiling src/emu/netplay/netplay_packet.cpp with
rem NETPLAY_STANDALONE so both ends cannot drift apart.
rem
rem     server\build.bat
rem
rem Output: build\server\netplay_server.exe

setlocal
set ROOT=%~dp0..
set VSDEV=C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars32.bat

if not exist "%VSDEV%" (
    echo build: cannot find vcvars32.bat at "%VSDEV%"
    exit /b 1
)

call "%VSDEV%" >nul
if errorlevel 1 exit /b 1

if not exist "%ROOT%\build\server" mkdir "%ROOT%\build\server"

rem /I src\emu so that #include "netplay/netplay_packet.h" resolves the same
rem way it does inside the emu project.
cl /nologo /EHsc /O2 /W3 /DNETPLAY_STANDALONE ^
   /I "%ROOT%\src\emu" ^
   /Fo"%ROOT%\build\server\\" ^
   /Fe"%ROOT%\build\server\netplay_server.exe" ^
   "%ROOT%\server\netplay_server.cpp" ^
   "%ROOT%\src\emu\netplay\netplay_packet.cpp" ^
   ws2_32.lib

if errorlevel 1 (
    echo build: FAILED
    exit /b 1
)

echo build: build\server\netplay_server.exe
endlocal

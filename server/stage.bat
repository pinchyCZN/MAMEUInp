@echo off
rem Gather everything the Debian box needs into one directory, ready to scp.
rem
rem     server\stage.bat            stages to B:\nplay\server\dist
rem     server\stage.bat D:\out     stages somewhere else
rem
rem COPIES, NOT ORIGINALS. netplay_packet.* and netplay_types.h belong to
rem src\emu\netplay and are shared with the emulator. Re-run this after any
rem change to them: a stale copy on the VPS is two builds disagreeing about
rem the wire format, which shows up as every packet being counted malformed.

setlocal
set ROOT=%~dp0..
set OUT=%~1
if "%OUT%"=="" set OUT=B:\nplay\server\dist

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\netplay" 2>nul

copy /y "%ROOT%\server\netplay_server.cpp"            "%OUT%\"               >nul
copy /y "%ROOT%\server\Makefile"                      "%OUT%\"               >nul
copy /y "%ROOT%\server\build.sh"                      "%OUT%\"               >nul
copy /y "%ROOT%\server\README-linux.txt"              "%OUT%\"               >nul
copy /y "%ROOT%\src\emu\netplay\netplay_packet.cpp"   "%OUT%\netplay\"       >nul
copy /y "%ROOT%\src\emu\netplay\netplay_packet.h"     "%OUT%\netplay\"       >nul
copy /y "%ROOT%\src\emu\netplay\netplay_types.h"      "%OUT%\netplay\"       >nul

echo staged to %OUT%
echo.
dir /b /s "%OUT%"
echo.
echo Send it with:
echo     scp -r "%OUT%" user@host:~/nplay-server
endlocal

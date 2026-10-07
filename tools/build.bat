@echo off
rem MAMEUI netplay - the ONLY way an agent compiles this tree.
rem
rem     tools\build.bat                     Release Win32, capped
rem     tools\build.bat Debug               a configuration
rem     tools\build.bat Release emu         one project
rem     set NP_JOBS=1 & tools\build.bat     lower the cap further
rem
rem TWO PROJECTS AT A TIME, NO MULTIPROCESSOR COMPILATION. CLAUDE.md rule 12.
rem That matches the user's own Options -> Projects and Solutions -> Build and
rem Run setting, so an agent build loads the machine no more than their own
rem does. DO NOT RAISE IT.
rem
rem WHY THIS EXISTS: a bare "msbuild /m" on this solution spawned 46 cl.exe
rem and made the machine unusable for anything else. That is TWO multipliers,
rem and capping either one alone does not help:
rem
rem   /m with no number   MSBuild runs as many PROJECTS at once as there are
rem                       cores. There are 131 projects.
rem   /MP                 genie sets MultiProcessorCompilation=true on every
rem                       one of them for Release (3rdparty/genie
rem                       vstudio_vcxproj.lua), so each project's cl.exe
rem                       forks per core as well.
rem
rem SETTING THIS IN THE MSVC GUI DOES NOT HOLD. The vcxproj files under
rem build/projects are GENERATED; "make vs2017 ..." overwrites them and the
rem setting with them. The command line is the only durable place.
rem
rem Full-rebuild time is dominated by the fact that precompiled headers are
rem forcibly off for MSVC, not by this cap - notes/build-config.txt 3.

setlocal

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release

set PROJECT=%~2

if "%NP_JOBS%"=="" set NP_JOBS=2

set SLN=build\projects\winui\mamecoinop\vs2017\mamecoinop.sln
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvarsall.bat

cd /d %~dp0..

if not exist "%SLN%" (
    echo build: no solution at %SLN%
    echo        generate it first - see CLAUDE.md, Build configuration
    exit /b 1
)

if not exist "%VCVARS%" (
    echo build: cannot find vcvarsall.bat at "%VCVARS%"
    exit /b 1
)

rem NEVER TWO BUILDS AT ONCE. Two msbuilds write the same obj tree and the
rem loser dies with "Cannot open compiler generated file: Permission denied".
rem Worse, a cl.exe killed mid-write leaves a truncated .obj that the next
rem build treats as up to date, so the damage outlives the collision.
rem
rem This also catches the user building in the IDE, which is the case that
rem matters: devenv spawns msbuild.exe and cl.exe like anything else.
rem FULL PATH TO find.exe ON PURPOSE. A bare "find" picks up Git Bash's
rem find(1) when this is launched from that shell, which takes /I as a path,
rem prints an error and leaves ERRORLEVEL saying nothing useful - so the
rem guard silently passed and two builds ran over one obj tree anyway.
set FIND=%SystemRoot%\System32\find.exe

rem cl.exe ONLY, NOT msbuild.exe. Visual Studio keeps MSBuild.exe node
rem processes alive and idle long after a build finishes (node reuse), so
rem msbuild.exe means "the IDE is open", not "a build is running" - testing
rem it refuses every build while VS is open, which is useless. A live cl.exe
rem is the thing that actually says somebody is compiling.
tasklist /FI "IMAGENAME eq cl.exe" 2>nul | "%FIND%" /I "cl.exe" >nul
if not errorlevel 1 goto :busy_cl
goto :not_busy

:busy_cl
echo build: REFUSING - cl.exe is already running.
echo        Another build is in progress, possibly in Visual Studio.
echo        Wait for it to finish; do not run two builds over one obj tree.
exit /b 1

:not_busy

call "%VCVARS%" x86 -vcvars_ver=14.13 >nul
if errorlevel 1 (
    echo build: vcvarsall failed
    exit /b 1
)

rem TWO caps, and both are needed.
rem
rem   /m:N                     bounds how many PROJECTS run at once.
rem   cap_mp.props             turns /MP off inside each one. A plain
rem                            /p:MultiProcessorCompilation=false does NOT
rem                            work - genie writes that setting into an
rem                            ItemDefinitionGroup, which makes it ClCompile
rem                            item metadata, and a global property does not
rem                            override item metadata. The props file sets it
rem                            from inside a target, which does.
rem
rem ForceImportAfterCppTargets needs an absolute path.
set CAPS=/m:%NP_JOBS% /p:ForceImportAfterCppTargets=%CD%\tools\cap_mp.props

rem GOTO RATHER THAN IF/ELSE BLOCKS, and no parentheses in any echo inside
rem one. An unescaped ")" in "process(es)" closed the if-block early, so the
rem else-branch ran too and a second msbuild started with an empty /t: -
rem which failed with MSB1004 after the real build had already succeeded.
if not "%PROJECT%"=="" goto :one_project

echo build: %CONFIG% Win32, at most %NP_JOBS% compiler process
msbuild "%SLN%" /p:Configuration=%CONFIG% /p:Platform=Win32 %CAPS% /v:minimal /nologo
goto :done

:one_project
echo build: %CONFIG% Win32 project %PROJECT%, at most %NP_JOBS% compiler process
msbuild "%SLN%" /p:Configuration=%CONFIG% /p:Platform=Win32 /t:%PROJECT% %CAPS% /v:minimal /nologo

:done
set RC=%ERRORLEVEL%
echo build: exit %RC%
exit /b %RC%

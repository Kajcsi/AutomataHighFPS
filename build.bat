@echo off
rem Builds dinput8.dll with the Visual Studio 2022 x64 toolchain.
setlocal
set VCVARS=
for %%V in (2022 18) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\Community\VC\Auxiliary\Build\vcvars64.bat" (
        if not defined VCVARS set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%V\Community\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS (
    echo Visual Studio with the C++ workload was not found.
    exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /GS- /LD /Fobuild\ /Fdbuild\ ^
    src\dllmain.cpp src\log.cpp src\config.cpp src\memory.cpp src\game.cpp src\timing.cpp src\display.cpp src\subsystems.cpp src\patchkit.cpp src\flight.cpp src\hacking.cpp src\movement.cpp src\cloth.cpp src\enemies.cpp src\world.cpp src\camera.cpp src\exact.cpp src\anim.cpp ^
    /link /DEF:src\dinput8.def /OUT:build\dinput8.dll /IMPLIB:build\dinput8.lib user32.lib || exit /b 1
echo Built build\dinput8.dll

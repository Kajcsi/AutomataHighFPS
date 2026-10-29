@echo off
rem Builds tests\build\harness.exe (offline patch tests against a decrypted game exe).
setlocal
set VCVARS=
for %%V in (2022 18) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\Community\VC\Auxiliary\Build\vcvars64.bat" (
        if not defined VCVARS set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%V\Community\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS exit /b 1
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /Od /Zi /MT /EHsc /std:c++17 /W3 /GS- /Fobuild\ /Fdbuild\ ^
    harness.cpp subsystem_tests.cpp ..\src\log.cpp ..\src\config.cpp ..\src\memory.cpp ..\src\game.cpp ^
    ..\src\timing.cpp ..\src\display.cpp ..\src\subsystems.cpp ..\src\patchkit.cpp ..\src\flight.cpp ..\src\hacking.cpp ..\src\movement.cpp ^
    /link /OUT:build\harness.exe /DYNAMICBASE /HIGHENTROPYVA user32.lib || exit /b 1

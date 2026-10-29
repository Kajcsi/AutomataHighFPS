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
    ..\src\timing.cpp ..\src\display.cpp ..\src\subsystems.cpp ..\src\patchkit.cpp ..\src\flight.cpp ..\src\hacking.cpp ..\src\movement.cpp ..\src\cloth.cpp ..\src\enemies.cpp ..\src\world.cpp ..\src\camera.cpp ..\src\exact.cpp ..\src\anim.cpp ..\src\misc.cpp ..\src\camera_final.cpp ..\src\enemy_final.cpp ^
    /link /OUT:build\harness.exe /MAP:build\harness.map /DYNAMICBASE /HIGHENTROPYVA user32.lib || exit /b 1

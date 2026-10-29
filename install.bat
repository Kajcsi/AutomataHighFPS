@echo off
rem Copies the mod into the game folder (the folder above this one).
setlocal
cd /d "%~dp0"
if not exist build\dinput8.dll (
    echo build\dinput8.dll not found - run build.bat first.
    exit /b 1
)
if exist ..\dinput8.dll (
    fc /b build\dinput8.dll ..\dinput8.dll >nul 2>&1 || (
        if not exist ..\dinput8.dll.bak copy /y ..\dinput8.dll ..\dinput8.dll.bak >nul
        echo Existing dinput8.dll backed up to dinput8.dll.bak
    )
)
copy /y build\dinput8.dll ..\dinput8.dll >nul || exit /b 1
if not exist ..\AutomataHighFPS.ini copy /y AutomataHighFPS.ini ..\AutomataHighFPS.ini >nul
echo Installed. Settings: %~dp0..\AutomataHighFPS.ini

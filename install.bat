@echo off
rem Copies the mod into the game folder (the folder above this one).
setlocal
cd /d "%~dp0"
if not exist build\dinput8.dll (
    echo build\dinput8.dll not found - run build.bat first.
    exit /b 1
)
rem Back up a dinput8.dll that belongs to something else (the mod's own settings
rem file marks an existing install of this mod, which is simply replaced).
if exist ..\dinput8.dll if not exist ..\AutomataHighFPS.ini if not exist ..\dinput8.dll.bak (
    copy /y ..\dinput8.dll ..\dinput8.dll.bak >nul
    echo Existing dinput8.dll backed up to dinput8.dll.bak
)
copy /y build\dinput8.dll ..\dinput8.dll >nul || exit /b 1
if not exist ..\AutomataHighFPS.ini copy /y AutomataHighFPS.ini ..\AutomataHighFPS.ini >nul
echo Installed. Settings: %~dp0..\AutomataHighFPS.ini

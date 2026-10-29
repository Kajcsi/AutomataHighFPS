@echo off
rem Removes the mod from the game folder (settings file is kept).
setlocal
cd /d "%~dp0"
if exist ..\dinput8.dll del /q ..\dinput8.dll
if exist ..\dinput8.dll.bak move /y ..\dinput8.dll.bak ..\dinput8.dll >nul
if exist ..\AutomataHighFPS.log del /q ..\AutomataHighFPS.log
echo Uninstalled.

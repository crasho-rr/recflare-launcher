@echo off
rem Builds build\2026loader.dll, then "build\RecFlare Launcher.exe" with it embedded.
call "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>nul
cd /d "%~dp0"
if not exist build mkdir build
cd build
cl /nologo /W4 /O2 /MT /EHa /std:c++17 /LD ..\loader\loader.cpp /link /OUT:2026loader.dll || exit /b 1
rc /nologo /fo launcher.res ..\src\launcher.rc || exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE ..\src\launcher.cpp launcher.res /link /SUBSYSTEM:WINDOWS /OUT:"RecFlare Launcher.exe" user32.lib gdi32.lib gdiplus.lib comctl32.lib shell32.lib shlwapi.lib ole32.lib bcrypt.lib dwmapi.lib advapi32.lib || exit /b 1

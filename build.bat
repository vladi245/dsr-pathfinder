@echo off
rem Builds DSRPathfinder.exe with the MSVC x64 compiler (Visual Studio or Build Tools).
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto compile
rem Not in a developer prompt: find Visual Studio and load its x64 environment.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
goto compile

:novs
echo Visual Studio C++ build tools not found.
exit /b 1

:compile
cd /d "%~dp0"
rem The guide and navmesh are built into the exe, so they have to exist first.
if not exist guide.txt goto noguide
if not exist navmesh.bin goto noguide
if not exist build mkdir build
rc /nologo /fo build\app.res src\app.rc
if errorlevel 1 exit /b 1
rem A windowed app: no console, it lives in the notification area.
cl /nologo /std:c++17 /O2 /EHsc /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"build\\" /Fe"DSRPathfinder.exe" ^
   src\main.cpp src\game.cpp src\memory.cpp src\guide.cpp src\nav.cpp src\overlay.cpp src\textfile.cpp ^
   src\camera.cpp src\route.cpp src\appdata.cpp src\tray.cpp build\app.res ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup user32.lib gdi32.lib gdiplus.lib winmm.lib shell32.lib ole32.lib
if errorlevel 1 exit /b 1
echo Built DSRPathfinder.exe
exit /b 0

:noguide
echo guide.txt and navmesh.bin go inside the exe: run "python tools\build_guide.py" first.
exit /b 1

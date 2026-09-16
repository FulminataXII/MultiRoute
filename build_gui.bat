@echo off
REM ============================================================================
REM MultiMux - GUI build
REM
REM Expected layout (this script lives at the repo root):
REM   MultiMux\
REM     build_gui.bat        <- this file
REM     build.bat
REM     src\                 <- all .cpp / .h
REM     third_party\imgui\   <- git clone --depth 1 https://github.com/ocornut/imgui.git
REM     build\               <- created by this script; everything generated lands here
REM       MultiMuxGui.exe    <- the only file needed to run
REM       obj\               <- intermediates (.obj/.pdb); safe to delete
REM       experiments\       <- created at runtime by the app
REM
REM Run from a "Developer Command Prompt for VS 2022".
REM   build_gui.bat          build
REM   build_gui.bat clean    delete the build folder and exit
REM
REM Ole32.lib and Avrt.lib are linked via #pragma comment in AudioEngine.cpp,
REM so they do not appear below.
REM ============================================================================

setlocal

set ROOT=%~dp0
set SRC=%ROOT%src
set IMGUI=%ROOT%third_party\imgui
set BUILD=%ROOT%build
set OBJ=%BUILD%\obj

if /I "%~1"=="clean" (
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    echo Cleaned.
    exit /b 0
)

if not exist "%SRC%\main_gui.cpp" (
    echo ERROR: sources not found at %SRC%
    echo Expected the .cpp/.h files in a "src" folder next to this script.
    exit /b 1
)

if not exist "%IMGUI%\imgui.cpp" (
    echo ERROR: Dear ImGui not found at %IMGUI%
    echo Run: git clone --depth 1 https://github.com/ocornut/imgui.git "%IMGUI%"
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%OBJ%" mkdir "%OBJ%"

REM /MT statically links the C runtime so the .exe runs on a machine without
REM the VC++ Redistributable installed - it makes build\MultiMuxGui.exe
REM genuinely self-contained, which is the point of shipping the folder.
REM
REM /wd4324 silences "structure was padded due to alignment specifier". That
REM padding is deliberate: PaddedAtomic and the log queue's Cell are
REM alignas(64) specifically to keep producer and consumer indices on separate
REM cache lines. The warning reports the feature working as intended.

cl /nologo /std:c++17 /EHsc /W4 /permissive- /MT /O2 ^
   /D_UNICODE /DUNICODE /DNDEBUG ^
   /wd4324 ^
   /I"%SRC%" /I"%IMGUI%" /I"%IMGUI%\backends" ^
   /Fo"%OBJ%\\" /Fd"%OBJ%\MultiMuxGui.pdb" ^
   "%SRC%\main_gui.cpp" ^
   "%SRC%\AudioEngine.cpp" ^
   "%SRC%\ExperimentRecorder.cpp" ^
   "%IMGUI%\imgui.cpp" ^
   "%IMGUI%\imgui_draw.cpp" ^
   "%IMGUI%\imgui_tables.cpp" ^
   "%IMGUI%\imgui_widgets.cpp" ^
   "%IMGUI%\backends\imgui_impl_win32.cpp" ^
   "%IMGUI%\backends\imgui_impl_dx11.cpp" ^
   d3d11.lib dxgi.lib ^
   /Fe:"%BUILD%\MultiMuxGui.exe"

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo BUILD FAILED
    exit /b %ERRORLEVEL%
)

echo.
echo Built %BUILD%\MultiMuxGui.exe
echo Ship the build folder ^(or just the .exe^) - no source or ImGui needed at runtime.

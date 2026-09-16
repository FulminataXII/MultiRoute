@echo off
REM ============================================================================
REM MultiMux - console build (no GUI, no ImGui dependency)
REM
REM Same layout as build_gui.bat; see that file for the folder diagram.
REM Produces build\MultiMux.exe.
REM
REM Note: the console frontend predates the volume and live-delay features and
REM does not expose them. It still routes audio correctly.
REM
REM Run from a "Developer Command Prompt for VS 2022".
REM   build.bat          build
REM   build.bat clean    delete the build folder and exit
REM ============================================================================

setlocal

set ROOT=%~dp0
set SRC=%ROOT%src
set BUILD=%ROOT%build
set OBJ=%BUILD%\obj

if /I "%~1"=="clean" (
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    echo Cleaned.
    exit /b 0
)

if not exist "%SRC%\main_cli.cpp" (
    echo ERROR: sources not found at %SRC%
    echo Expected the .cpp/.h files in a "src" folder next to this script.
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%OBJ%" mkdir "%OBJ%"

REM See build_gui.bat for why /MT and /wd4324 are used.

cl /nologo /std:c++17 /EHsc /W4 /permissive- /MT /O2 ^
   /D_UNICODE /DUNICODE /DNDEBUG ^
   /wd4324 ^
   /I"%SRC%" ^
   /Fo"%OBJ%\\" /Fd"%OBJ%\MultiMux.pdb" ^
   "%SRC%\main_cli.cpp" ^
   "%SRC%\AudioEngine.cpp" ^
   /Fe:"%BUILD%\MultiMux.exe"

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo BUILD FAILED
    exit /b %ERRORLEVEL%
)

echo.
echo Built %BUILD%\MultiMux.exe

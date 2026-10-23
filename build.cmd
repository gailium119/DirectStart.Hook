@echo off
setlocal
cd /d "%~dp0"

if not exist "third_party\libvalinet\valinet\pdb\pdb.h" goto init_submodules
if not exist "third_party\minhook\include\MinHook.h" goto init_submodules
goto build

:init_submodules
where git >nul 2>nul
if errorlevel 1 (
    echo git was not found. Install Git and run:
    echo   git submodule update --init
    exit /b 1
)
echo Initializing git submodules...
git submodule update --init
if errorlevel 1 exit /b 1

:build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Install Visual Studio 2022 with the C++ workload.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo MSBuild not found.
    exit /b 1
)

"%MSBUILD%" "%~dp0DirectStart.slnx" /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if errorlevel 1 exit /b 1

echo.
echo Built: %~dp0build\x64\Release\d3d11.dll

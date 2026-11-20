@echo off
rem Only for the MSVC fallback (the default build is clang, from a plain shell: cmake --preset clang).
rem Runs a command inside the MSVC x64 developer environment, e.g.
rem   scripts\dev.cmd cmake --preset msvc
rem   scripts\dev.cmd cmake --build --preset msvc-debug
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio with C++ tools not found. 1>&2
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
%*

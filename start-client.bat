@echo off
rem More clients on the title screen against the running dev shard (start-dev.bat's; this starts no server).
rem   start-client.bat [debug|release] [count]   e.g. start-client.bat debug 2: two clients
setlocal
if exist "%~dp0dev.local.bat" call "%~dp0dev.local.bat"
set "CONFIG=Debug"
if /i "%~1"=="release" set "CONFIG=RelWithDebInfo"
set "COUNT=%~2"
if "%COUNT%"=="" set "COUNT=1"
set "EXE=%~dp0build\clang\client\%CONFIG%\SroReimagined.exe"
set "PEM=%~dp0build\clang\server\%CONFIG%\dev\server.pem"
if not exist "%EXE%" (
    echo %EXE% not found - build first: cmake --build --preset %CONFIG:RelWithDebInfo=release%
    exit /b 1
)
if not exist "%PEM%" (
    echo %PEM% not found - start the dev shard first: start-dev.bat
    exit /b 1
)
if not defined SR_GAME_DIR set "SR_GAME_DIR=E:\Silkroad\VSRO_Client"
for /l %%i in (1,1,%COUNT%) do (
    start "SroReimagined %%i" /D "%~dp0" "%EXE%" --screen title --server-certificate "%PEM%"
    rem A moment between them, so they do not read the archives at the same instant.
    timeout /t 2 /nobreak >nul
)
endlocal

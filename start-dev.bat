@echo off
rem Starts a development shard: the front (SroControl: the gateway, the login server and the shard's watch), two game
rem servers of one shard (East and West, deploy\dev-shard.cfg: what players share must work across game servers) and the
rem website's API (SroWeb), each in its own window, against a local PostgreSQL database; then the client on the title
rem screen. Stop them with stop-dev.bat. Build first: cmake --build --preset debug (or release).
rem
rem   start-dev.bat [debug|release] [noclient]
rem
rem The machine's own settings come from the environment or from dev.local.bat beside this file (git-ignored; set them
rem there, it is called first):
rem   SR_DB_URL         the database, postgresql://USER:PASSWORD@127.0.0.1:5432/DB (required)
rem   SR_GAME_DIR       the 1.188 game files (default E:\Silkroad\VSRO_Client)
rem   SR_VSRO_EXPORT    an export of the original's databases (docs/SERVER_SETUP.md): imported once when the database
rem                     lacks it; without it our layout alone
rem   SR_QUEST_SCRIPT   the original's quest script (Quest.sct): its quests and the ones our servers code written into
rem                     sr_quests at each start (the coded ones change with the code)
rem   SR_GAME_EXTRA     more options for both game servers (e.g. --qin-time-scale 60)
rem
rem The servers share one development certificate (dev\server.pem next to them), made here on first run; the client
rem trusts that file. A test account tester / secret123 is made if missing.
setlocal
if exist "%~dp0dev.local.bat" call "%~dp0dev.local.bat"
set "CONFIG=Debug"
set "PRESET=debug"
if /i "%~1"=="release" set "CONFIG=RelWithDebInfo"
if /i "%~1"=="release" set "PRESET=release"
set "BIN=%~dp0build\clang\server\%CONFIG%"
if not exist "%BIN%\SroGame.exe" (
    echo The servers are not built: cmake --build --preset %PRESET%
    exit /b 1
)
if not defined SR_DB_URL (
    echo SR_DB_URL is not set: set it in dev.local.bat beside this file, e.g.
    echo   set "SR_DB_URL=postgresql://sro:PASSWORD@127.0.0.1:5432/sro_dev"
    exit /b 1
)
if not defined SR_GAME_DIR set "SR_GAME_DIR=E:\Silkroad\VSRO_Client"

rem Full paths: some systems do not run programs from the current folder by name alone.
pushd "%BIN%"
"%BIN%\SroLogin.exe" --make-certificate || goto failed
rem The database: the original's export imported once when it is given, else our layout alone; then the quests.
if defined SR_VSRO_EXPORT (
    "%BIN%\SroDb.exe" --import "%SR_VSRO_EXPORT%" --if-missing || goto failed
) else (
    "%BIN%\SroDb.exe" --migrate || goto failed
)
if defined SR_QUEST_SCRIPT "%BIN%\SroDb.exe" --quests "%SR_QUEST_SCRIPT%" || goto failed
"%BIN%\SroLogin.exe" --create-account tester --password secret123 >nul 2>&1
rem Every local client comes from 127.0.0.1: no limit of login attempts per address here.
start "SroControl" /D "%BIN%" "%BIN%\SroControl.exe" --config "%~dp0deploy\dev-shard.cfg" --address-attempts 0
start "SroGame 1" /D "%BIN%" "%BIN%\SroGame.exe" --config "%~dp0deploy\dev-shard.cfg" --dev-commands --id 1 %SR_GAME_EXTRA%
start "SroGame 2" /D "%BIN%" "%BIN%\SroGame.exe" --config "%~dp0deploy\dev-shard.cfg" --dev-commands --id 2 %SR_GAME_EXTRA%
start "SroWeb" /D "%BIN%" "%BIN%\SroWeb.exe" --config "%~dp0deploy\dev-shard.cfg"
popd
rem The client on the title screen, trusting the servers' certificate ("start-dev.bat debug noclient" skips it).
set "CLIENT=%~dp0build\clang\client\%CONFIG%\SroReimagined.exe"
if /i not "%~2"=="noclient" if exist "%CLIENT%" start "SroReimagined" /D "%~dp0" "%CLIENT%" --screen title --server-certificate "%BIN%\dev\server.pem"
echo Gateway 127.0.0.1:15779, login :15780, game servers :15781 and :15782, the site's API :15790 (account tester / secret123).
echo The certificate the client trusts: %BIN%\dev\server.pem
exit /b 0

:failed
popd
echo Could not prepare the servers (database or certificate); see the message above.
exit /b 1

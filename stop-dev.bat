@echo off
rem Stops the servers and clients start-dev.bat started (by name: every one on this machine). A game server that does not
rem say goodbye is listed as offline when its last report is 15 s old, and its sessions are cleared when it starts again.
taskkill /IM SroControl.exe /IM SroGateway.exe /IM SroLogin.exe /IM SroGame.exe /IM SroWeb.exe /IM SroReimagined.exe /F >nul 2>&1
echo Servers stopped.

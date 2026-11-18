@echo off
setlocal
if not exist "%~dp0Warriors Orochi\WarriorsOrochi-Setup.exe" goto missing
start "" "%~dp0Warriors Orochi\WarriorsOrochi-Setup.exe"
exit /b 0
:missing
echo Nao foi possivel encontrar o Setup do port.
pause
exit /b 2

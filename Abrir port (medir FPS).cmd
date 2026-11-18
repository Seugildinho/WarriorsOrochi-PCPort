@echo off
setlocal
title Warriors Orochi - Port experimental (medindo FPS)
if not exist "%~dp0Warriors Orochi\WarriorsOrochi.exe" goto missing
if not exist "%~dp0logs" mkdir "%~dp0logs"
set "OROCHI_TEST_START="
set "OROCHI_TEST_INPUTS="
set "OROCHI_TEST_CONTROL_FILE="
set "OROCHI_CAPTURE_DIR="
set "OROCHI_CAPTURE_RAM="
set "PS2X_IOP_RPC_TRACE_SID="
set "OROCHI_DIAG_DUMPS="
set "OROCHI_IPU_DECODER="
set "OROCHI_IPU_VERIFY="
set "OROCHI_GPU_VERIFY="
set "OROCHI_GPU_PROFILE="
set "OROCHI_LIGHTWEIGHT_DIAGNOSTICS="
set "OROCHI_DIAGNOSTICS="
set "OROCHI_RENDERER="
set "OROCHI_WIDESCREEN="
set "OROCHI_PCSX2_SCALE="
set "OROCHI_PCSX2_FIXES="
set "OROCHI_SETUP="
set "OROCHI_AUTOSAVE="
set "OROCHI_SPU2="
set "OROCHI_SPU2_MUTE="
set "OROCHI_SPU2_WAV="
rem Medicao: velocidade e tempos do desenho a cada 2 s em logs\manual.stdout.log.
set "OROCHI_DIAGNOSTICS=1"
set "OROCHI_LIGHTWEIGHT_DIAGNOSTICS=1"
set "OROCHI_GPU_TIMING=1"
set "OROCHI_GPU_PROFILE=1"
rem Na parte lenta, F9 salva o quadro e a memoria de video em logs.
set "OROCHI_GIF_CAPTURE=%~dp0logs\captura-fps.bin"
echo Iniciando o port experimental com medicao de FPS. Aguarde a abertura do jogo.
echo Teclado: Enter = Start, setas = navegar, X = confirmar, C = voltar.
echo Na parte em que o FPS cair, aperte F9 uma vez. O titulo da janela confirma a captura.
echo Para sair, feche a janela do jogo.
"%~dp0Warriors Orochi\WarriorsOrochi.exe" 1>"%~dp0logs\manual.stdout.log" 2>"%~dp0logs\manual.stderr.log"
if errorlevel 1 goto failed
exit /b 0
:missing
echo Nao foi possivel encontrar o executavel do port.
pause
exit /b 2
:failed
echo O port encerrou com erro. Os detalhes estao na pasta logs:
echo %~dp0logs\manual.stderr.log
pause
exit /b 1

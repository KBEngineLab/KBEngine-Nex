@echo off
cd ..
set PROJECT_PATH=%cd%
for /r %%i in (site-packages) do @if exist "%%i" set KBE_VENV_PATH=%%i

cd ..

if not defined KBE_ROOT (
    set KBE_ROOT=%cd%
)

set KBE_RES_PATH=%KBE_ROOT%/kbe/res/;%PROJECT_PATH%/;%PROJECT_PATH%/res/
set KBE_BIN_PATH=%KBE_ROOT%/kbe/bin/server/


cd %~dp0

echo PROJECT_PATH = %PROJECT_PATH%
echo KBE_ROOT = %KBE_ROOT%
echo KBE_RES_PATH = %KBE_RES_PATH%
echo KBE_BIN_PATH = %KBE_BIN_PATH%
echo KBE_VENV_PATH = %KBE_VENV_PATH%

"%KBE_BIN_PATH%/kbcmd.exe" --getuid > nul
if not defined uid set uid=%errorlevel%
echo UID = %uid%

if defined KBE_ROOT (python "%KBE_ROOT%/kbe\tools\server\pycluster\cluster_controller.py" stop %uid%) else (python "..\kbe\tools\server\pycluster\cluster_controller.py" stop %uid%)

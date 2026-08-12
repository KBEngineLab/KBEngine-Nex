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

if defined uid (echo UID = %uid%)

cd %~dp0

call "kill_server.bat"

echo PROJECT_PATH = %PROJECT_PATH%
echo KBE_ROOT = %KBE_ROOT%
echo KBE_RES_PATH = %KBE_RES_PATH%
echo KBE_BIN_PATH = %KBE_BIN_PATH%
echo KBE_VENV_PATH = %KBE_VENV_PATH%

start "" "%KBE_BIN_PATH%/machine.exe" --cid=1000 --gus=1 --hide=1
start "" "%KBE_BIN_PATH%/logger.exe" --cid=2000 --gus=2 --hide=1
start "" "%KBE_BIN_PATH%/interfaces.exe" --cid=3000 --gus=3 --hide=1
start "" "%KBE_BIN_PATH%/dbmgr.exe" --cid=4000 --gus=4 --hide=1
start "" "%KBE_BIN_PATH%/baseappmgr.exe" --cid=5000 --gus=5 --hide=1
start "" "%KBE_BIN_PATH%/cellappmgr.exe" --cid=6000 --gus=6 --hide=1
start "" "%KBE_BIN_PATH%/baseapp.exe" --cid=7001 --gus=7 --hide=1
@rem start "" "%KBE_BIN_PATH%/baseapp.exe" --cid=7002 --gus=8 --hide=1
start "" "%KBE_BIN_PATH%/cellapp.exe" --cid=8001 --gus=9 --hide=1
@rem start "" "%KBE_BIN_PATH%/cellapp.exe" --cid=8002  --gus=10 --hide=1
start "" "%KBE_BIN_PATH%/loginapp.exe" --cid=9000 --gus=11 --hide=1

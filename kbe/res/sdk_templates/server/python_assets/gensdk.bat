@echo off
set curpath=%~dp0

cd ..
set KBE_ROOT=%cd%
set KBE_RES_PATH=%KBE_ROOT%/kbe/res/;%curpath%/;%curpath%/scripts/;%curpath%/res/
set KBE_BIN_PATH=%KBE_ROOT%/kbe/bin/server/

if defined uid (echo UID = %uid%)

echo KBE_ROOT = %KBE_ROOT%
echo KBE_RES_PATH = %KBE_RES_PATH%
echo KBE_BIN_PATH = %KBE_BIN_PATH%

cd %curpath%

rem 四端必须顺序生成，任一失败立即返回，避免异步窗口掩盖不完整产物。
rem Generate the four supported SDKs sequentially and fail fast so asynchronous windows cannot hide incomplete output.
"%KBE_BIN_PATH%/kbcmd.exe" --clientsdk=csharp --outpath="%curpath%/kbengine_csharp_sdk"
if %errorlevel% neq 0 exit /b %errorlevel%
"%KBE_BIN_PATH%/kbcmd.exe" --clientsdk=cxx --outpath="%curpath%/kbengine_cxx_sdk"
if %errorlevel% neq 0 exit /b %errorlevel%
"%KBE_BIN_PATH%/kbcmd.exe" --clientsdk=typescript --outpath="%curpath%/kbengine_typescript_sdk"
if %errorlevel% neq 0 exit /b %errorlevel%
"%KBE_BIN_PATH%/kbcmd.exe" --clientsdk=gdscript --outpath="%curpath%/kbengine_gdscript_sdk"
if %errorlevel% neq 0 exit /b %errorlevel%

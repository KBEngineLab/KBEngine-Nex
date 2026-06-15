@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion


REM =========================================
REM Git
REM =========================================

git --version >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [Error] Git is not installed or not in PATH.
    echo Please install Git from https://git-scm.com/downloads
    exit /b 1
) else (
    echo [Success] Git is installed.
)



REM =========================================
REM Check GitHub network accessibility (Git clone test)
REM =========================================
echo [Checking] Trying to access GitHub ...
set "TMP_TEST_DIR=%TEMP%\gh_test"
if exist "%TMP_TEST_DIR%" rd /s /q "%TMP_TEST_DIR%"
mkdir "%TMP_TEST_DIR%"

git ls-remote https://github.com/microsoft/vcpkg.git >nul 2>nul
if errorlevel 1 (
    echo [Error] Cannot access GitHub repository!
    echo         There may be a network issue or proxy required. Please resolve network issues before running the script.
    echo.
    echo         There is also a domestic proxy mirror. You can try running install\proxy\install_windows.bat.
    rd /s /q "%TMP_TEST_DIR%" >nul 2>nul
    if not defined CI (
        pause
    )
    exit /b 1
)

rd /s /q "%TMP_TEST_DIR%" >nul 2>nul
echo [Success] GitHub repository is accessible

REM =========================================
REM Default parameters
REM =========================================
set "CONFIG=Debug"
set "PLATFORM=x64"
set "PROJECT_ROOT=%~dp0..\"
set "INIT_BUILD_PROJ=%PROJECT_ROOT%kbe\src\server\init\init.vcxproj"
set "SOLUTION_FILE=%PROJECT_ROOT%kbe\src\kbengine nex.sln"
set "GUICONSOLE_SOLUTION_FILE=%PROJECT_ROOT%kbe\src\guiconsole.sln"
set "LOG_FILE=%~dp0build.log"
set "VCPKG_PATH="

REM =========================================
REM Parse parameters
REM =========================================
if "%~1"=="" (
    set "CONFIG=Debug"
) else if /i "%~1"=="Debug" (
    set "CONFIG=Debug"
) else if /i "%~1"=="Release" (
    set "CONFIG=Release"
) else if /i "%~1"=="help" (
    goto showHelp
) else (
    echo [Error] Invalid CONFIG parameter: %~1
    exit /b 1
)


if not "%~2"=="" (
    set "VCPKG_PATH=%~2"
    if not exist "!VCPKG_PATH!\vcpkg.exe" (
        echo [Error] The specified vcpkg path is invalid: !VCPKG_PATH!
        exit /b 1
    )
)

:showHelp
echo =========================================
echo Usage:
echo   build.bat [CONFIG] [VCPKGPATH]
echo.
echo Available parameters:
echo   CONFIG=Debug^|Release      Specify build configuration, default is Debug
echo   VCPKGPATH=Path             Specify vcpkg installation path
echo   GUICONSOLE                 Install GUICONSOLE
echo =========================================
echo.
if "%~1"=="help" exit /b 0

REM =========================================
REM 1. Check vcpkg (modified)
REM =========================================
echo.
echo [Checking] Searching for vcpkg...

set "VCPKG_EXE="

REM 1) Use parameter if provided
if defined VCPKG_PATH (
    set "VCPKG_EXE=!VCPKG_PATH!\vcpkg.exe"
    if not exist "!VCPKG_EXE!" (
        echo [Error] The specified vcpkg path is invalid: !VCPKG_PATH!
        exit /b 1
    )
) else (
    REM 2) Search default installation directory
    if exist "%USERPROFILE%\AppData\Local\kbe-vcpkg\.git\" (


        set "VCPKG_EXE=%USERPROFILE%\AppData\Local\kbe-vcpkg\vcpkg.exe"
        set "VCPKG_PATH=%USERPROFILE%\AppData\Local\kbe-vcpkg"


        git -C "%USERPROFILE%\AppData\Local\kbe-vcpkg" reset --hard HEAD
        git -C "%USERPROFILE%\AppData\Local\kbe-vcpkg" pull

        call "!VCPKG_PATH!\bootstrap-vcpkg.bat"

        goto :found_vcpkg
    )

    REM 3) Not found, prompt and install to default directory
    echo.
    echo [Notice] kbe-vcpkg not detected
    
    echo [Downloading] Start downloading and installing vcpkg...
    set "VCPKG_PATH=%USERPROFILE%\AppData\Local\kbe-vcpkg"
    echo VCPKG_PATH=!VCPKG_PATH!
    git clone https://github.com/microsoft/vcpkg "!VCPKG_PATH!"
    if errorlevel 1 (
        echo [Error] vcpkg download failed
        exit /b 1
    )
    set "VCPKG_EXE=!VCPKG_PATH!\vcpkg.exe"
    call "!VCPKG_PATH!\bootstrap-vcpkg.bat"
)

:found_vcpkg


echo [Found] vcpkg path: %VCPKG_EXE%
echo [Executing] vcpkg integrate install ...
"%VCPKG_EXE%" integrate install
if errorlevel 1 (
    echo [Error] vcpkg integrate install failed
    exit /b 1
)

echo [Executing] vcpkg install x64-windows-static manifest dependencies ...
pushd "%PROJECT_ROOT%kbe\src"
"%VCPKG_EXE%" install --triplet x64-windows-static --x-install-root="%PROJECT_ROOT%kbe\src\vcpkg_installed\x64-windows-static"
set "VCPKG_INSTALL_RESULT=%ERRORLEVEL%"
popd
if not "%VCPKG_INSTALL_RESULT%"=="0" (
    echo [Error] vcpkg install failed
    exit /b 1
)

set "VCPKG_MSBUILD_PROPS=/p:VcpkgUseStatic=true /p:VcpkgTriplet=x64-windows-static /p:VcpkgManifestInstall=false /p:VcpkgApplocalDeps=false"

REM =========================================
REM 2. Find Visual Studio installation path and all MSVC toolsets
REM =========================================
echo.
echo [Checking] Searching for Visual Studio installation path...

set "VSWHERE_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE_PATH%" (
    echo [Error] vswhere.exe not found. Please ensure Visual Studio or Build Tools is installed.
    if not defined CI (
        pause
    )
    exit /b 1
)

set "VS_INSTALL_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE_PATH%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_INSTALL_PATH=%%i"
)



if not defined VS_INSTALL_PATH (
    echo [Error] No Visual Studio with C++ toolset found
    if not defined CI (
        pause
    )
    exit /b 1
)

echo [Found] Visual Studio path: %VS_INSTALL_PATH%
set "MSVC_ROOT=%VS_INSTALL_PATH%\VC\Tools\MSVC"



for /f "delims=" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property catalog_productLineVersion') do set VS_MAJOR=%%i

set "PLATFORM_TOOLSET="

if "%VS_MAJOR%"=="2015" set "PLATFORM_TOOLSET=-p:PlatformToolset=v140"
if "%VS_MAJOR%"=="2017" set "PLATFORM_TOOLSET=-p:PlatformToolset=v141"
if "%VS_MAJOR%"=="2019" set "PLATFORM_TOOLSET=-p:PlatformToolset=v142"
if "%VS_MAJOR%"=="2022" set "PLATFORM_TOOLSET=-p:PlatformToolset=v143"


echo VS_MAJOR: %VS_MAJOR%
echo PLATFORM_TOOLSET: %PLATFORM_TOOLSET%



REM List all MSVC toolset versions
set "MSVC_COUNT=0"
set "VCVARS_VAR="
set "MSVC_VER_VAR="
for /d %%v in ("%MSVC_ROOT%\*") do (
    set /a MSVC_COUNT+=1
    set "MSVC_VER_!MSVC_COUNT!=%%~nxv"
)

echo.
if %MSVC_COUNT%==0 (
    echo [Warning] No MSVC toolset found, using default
) else if %MSVC_COUNT%==1 (
    set "MSVC_VER=!MSVC_VER_1!"
    echo [Selected] Using MSVC toolset version: !MSVC_VER!

    set "MSVC_FULL_PATH=%MSVC_ROOT%\!MSVC_VER!"
    set "VCVARS_VAR=-vcvars_ver=!MSVC_VER!"
    set "MSVC_VER_VAR=-p:VCToolsVersion=!MSVC_VER!"
) else (
    echo Found the following MSVC toolset versions:
    for /l %%i in (1,1,%MSVC_COUNT%) do (
        call echo   %%i:!MSVC_VER_%%i!
    )

    set "LATEST_VER=0.0.0"
    for /l %%i in (1,1,%MSVC_COUNT%) do (
        set "CUR=!MSVC_VER_%%i!"

        for /f "tokens=1-3 delims=." %%a in ("!CUR!") do (
            set /a CUR_MAJOR=%%a
            set /a CUR_MINOR=%%b
            set /a CUR_PATCH=%%c
        )
        for /f "tokens=1-3 delims=." %%a in ("!LATEST_VER!") do (
            set /a L_MAJOR=%%a
            set /a L_MINOR=%%b
            set /a L_PATCH=%%c
        )

        if !CUR_MAJOR! gtr !L_MAJOR! (
            set "LATEST_VER=!CUR!"
        ) else if !CUR_MAJOR! equ !L_MAJOR! (
            if !CUR_MINOR! gtr !L_MINOR! (
                set "LATEST_VER=!CUR!"
            ) else if !CUR_MINOR! equ !L_MINOR! (
                if !CUR_PATCH! gtr !L_PATCH! (
                    set "LATEST_VER=!CUR!"
                )
            )
        )
    )


    echo.
    echo Latest MSVC version is !LATEST_VER!

    @REM echo.
    @REM echo +------------------------------------------------------------------------------------------------------+
    @REM echo ^| [Note] Choose the version compatible with vcpkg, usually the latest
    @REM echo ^| [Note] If unsure, check KBEMain build logs for the compiler path, e.g.:
    @REM echo ^|        Compiler found: E:/vs/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe
    @REM echo ^| [Note] The version used by vcpkg is 14.44.35207
    @REM echo ^| [Note] Or remove extra toolsets and keep only one
    @REM echo +------------------------------------------------------------------------------------------------------+
    @REM echo.


    @REM set /p "CHOICE=Please select the MSVC toolset number (1-%MSVC_COUNT%): "
    @REM if "!CHOICE!"=="" set "CHOICE=1"
    @REM if !CHOICE! GTR !MSVC_COUNT! (
    @REM     echo [Error] Invalid input!
    @REM     exit /b 1
    @REM )
    
    @REM call set "MSVC_VER=%%MSVC_VER_!CHOICE!%%"

    set "MSVC_VER=!LATEST_VER!"
    echo [Selected] Using MSVC toolset version: !MSVC_VER!

    set "MSVC_FULL_PATH=%MSVC_ROOT%\!MSVC_VER!"
    set "VCVARS_VAR=-vcvars_ver=!MSVC_VER!"
    set "MSVC_VER_VAR=-p:VCToolsVersion=!MSVC_VER!"
)

echo %MSVC_FULL_PATH%
echo %VCVARS_VAR%
echo %MSVC_VER_VAR%


:: Extract first two version segments, e.g., 14.44
@REM for /f "tokens=1,2 delims=." %%a in ("%MSVC_VER%") do (
@REM     set "VC_VER=%%a.%%b"
@REM )

echo VC_VER: %MSVC_VER%

set "VCVARSALL_BAT=%VS_INSTALL_PATH%\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARSALL_BAT%" x64 -vcvars_ver=%MSVC_VER%
if errorlevel 1 (
    echo [Error] Cannot load Visual Studio build environment
    if not defined CI (
        pause
    )
    exit /b 1
)

echo cl.exe path: %PATH%
cl



REM =========================================
REM 3. Build projects
REM =========================================
echo.
echo KBEngine-Nex Build Script
echo Project path: %PROJECT_ROOT%
echo Build configuration: %CONFIG% ^| Platform: %PLATFORM%
echo Log file: %LOG_FILE%
echo.

echo [Step 1] Building KBEMain.vcxproj ...
msbuild "%INIT_BUILD_PROJ%" /p:Configuration=%CONFIG% %MSVC_VER_VAR% %PLATFORM_TOOLSET% %VCPKG_MSBUILD_PROPS% /p:Platform=%PLATFORM% /m    ^
    /fileLogger /fileLoggerParameters:LogFile=%LOG_FILE%;Append;Encoding=UTF-8 ^
    /consoleloggerparameters:DisableConsoleColor 
if errorlevel 1 (
    echo [Error] KBEMain.vcxproj build failed. Check %LOG_FILE%
    if not defined CI (
        pause
    )
    exit /b 1
)

if "%~3"=="GUICONSOLE" (
    echo [Installing] GUICONSOLE...
    goto GUICONSOLE
)

echo.
echo [Step 2] Building kbengine nex.sln ...
msbuild "%SOLUTION_FILE%" /p:Configuration=%CONFIG% %MSVC_VER_VAR%  %PLATFORM_TOOLSET% %VCPKG_MSBUILD_PROPS% /p:Platform=Win64 /m   ^
    /fileLogger /fileLoggerParameters:LogFile=%LOG_FILE%;Append;Encoding=UTF-8 ^
    /consoleloggerparameters:DisableConsoleColor
if errorlevel 1 (
    echo [Error] kbengine nex.sln build failed. Check %LOG_FILE%
    if not defined CI (
        pause
    )
    exit /b 1
)

echo.
echo [Success] All builds completed!
if not defined CI (
    pause
)
exit /b 0

:GUICONSOLE
echo.
echo [Step 2] Installing GUICONSOLE
msbuild "%GUICONSOLE_SOLUTION_FILE%" /p:Configuration=%CONFIG% %MSVC_VER_VAR%  %PLATFORM_TOOLSET% %VCPKG_MSBUILD_PROPS% /p:Platform=Win64 /m   ^
    /fileLogger /fileLoggerParameters:LogFile=%LOG_FILE%;Append;Encoding=UTF-8 ^
    /consoleloggerparameters:DisableConsoleColor
if errorlevel 1 (
    echo [Error] guiconsole.sln build failed. Check %LOG_FILE%
    if not defined CI (
        pause
    )
    exit /b 1
)

echo.
echo [Success] All builds completed!
if not defined CI (
    pause
)
exit /b 0

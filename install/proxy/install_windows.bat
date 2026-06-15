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
REM Check Proxy network accessibility (Git clone test)
REM =========================================
echo [Check] Trying to access Proxy ...
set "TMP_TEST_DIR=%TEMP%\gh_test"
if exist "%TMP_TEST_DIR%" rd /s /q "%TMP_TEST_DIR%"
mkdir "%TMP_TEST_DIR%"

git ls-remote https://gitcode.com/KBEngineLab/kbe-vcpkg-proxy.git >nul 2>nul
if errorlevel 1 (
    echo [Error] Failed to access Proxy repository!
    echo        Possibly a network issue, please fix your network and rerun the script.
    rd /s /q "%TMP_TEST_DIR%" >nul 2>nul
    pause
    exit /b 1
)

rd /s /q "%TMP_TEST_DIR%" >nul 2>nul
echo [Success] Proxy repository is accessible





REM =========================================
REM Default parameters
REM =========================================
set "CONFIG=Debug"
set "PLATFORM=x64"
set "PROJECT_ROOT=%~dp0..\..\" 
set "INIT_BUILD_PROJ=%PROJECT_ROOT%kbe\src\server\init\init.vcxproj"
set "SOLUTION_FILE=%PROJECT_ROOT%kbe\src\kbengine nex.sln"
set "GUICONSOLE_SOLUTION_FILE=%PROJECT_ROOT%kbe\src\guiconsole.sln"
set "LOG_FILE=%~dp0build.log"
set "VCPKG_PATH="

REM =========================================
REM Parse arguments
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
        echo [Error] Invalid vcpkgPath: !VCPKG_PATH!
        exit /b 1
    )
)

:showHelp
echo =========================================
echo Usage:
echo   build.bat [CONFIG] [VCPKGPATH]
echo.
echo Parameters:
echo   CONFIG=Debug^|Release      Specify build configuration, default is Debug
echo   VCPKGPATH=path             Specify vcpkg installation path
echo   GUICONSOLE                 Install GUICONSOLE
echo =========================================
echo.
if "%~1"=="help" exit /b 0





REM =========================================
REM 1. Detect vcpkg (modified version)
REM =========================================
echo.
echo [Check] Searching for vcpkg...

set "VCPKG_EXE="

REM 1) Use input parameter first
if defined VCPKG_PATH (
    set "VCPKG_EXE=!VCPKG_PATH!\vcpkg.exe"
    if not exist "!VCPKG_EXE!" (
        echo [Error] Invalid vcpkgPath: !VCPKG_PATH!
        exit /b 1
    )
) else (
    REM 2) Search in common installation directory for kbe-vcpkg
    if exist "%USERPROFILE%\AppData\Local\kbe-vcpkg-gitcode\vcpkg.exe" (
        set "VCPKG_EXE=%USERPROFILE%\AppData\Local\kbe-vcpkg-gitcode\vcpkg.exe"
        set "VCPKG_PATH=%USERPROFILE%\AppData\Local\kbe-vcpkg-gitcode"
        goto :found_vcpkg
    )

    REM 3) If not found, download and install to default directory
    echo.
    echo [Notice] kbe-vcpkg-gitcode not detected
    
    echo [Download] Start downloading vcpkg...
    set "VCPKG_PATH=%USERPROFILE%\AppData\Local\kbe-vcpkg-gitcode"
    echo VCPKG_PATH=!VCPKG_PATH!
    git clone https://gitcode.com/KBEngineLab/kbe-vcpkg-proxy.git "!VCPKG_PATH!"
    if errorlevel 1 (
        echo [Error] vcpkg download failed
        exit /b 1
    )
    set "VCPKG_EXE=!VCPKG_PATH!\vcpkg.exe"

)





:found_vcpkg

@REM Update kbe-vcpkg
git -C "%USERPROFILE%\AppData\Local\kbe-vcpkg-gitcode" reset --hard HEAD
git -C "%USERPROFILE%\AppData\Local\kbe-vcpkg-gitcode" pull




set DOWNLOADS_PATH=%USERPROFILE%\AppData\Local\kbe-vcpkg-gitcode\downloads

if not exist "%DOWNLOADS_PATH%" (
    echo Downloads directory not found, cloning repository...
    git clone https://gitcode.com/KBEngineLab/kbe-vcpkg-proxy-win64-download.git "%DOWNLOADS_PATH%"
    git -C "%DOWNLOADS_PATH%" lfs pull
) else (
    if not exist "%DOWNLOADS_PATH%\.git" (
        echo .git directory not found, removing existing downloads directory...
        rmdir /s /q "%DOWNLOADS_PATH%"
        echo Re-cloning repository...
        git clone https://gitcode.com/KBEngineLab/kbe-vcpkg-proxy-win64-download.git "%DOWNLOADS_PATH%"
    ) else (
        echo Downloads directory exists, updating...
        git -C "%DOWNLOADS_PATH%" pull
        git -C "%DOWNLOADS_PATH%" lfs pull
    )
)


@REM set "ARCH=%PROCESSOR_ARCHITECTURE%"
set "PATH=!VCPKG_PATH!;%PATH%"

@REM if not exist "!VCPKG_PATH!\downloads\tools\powershell-core-7.2.24-windows" (

@REM     if /i  not "%ARCH%"=="ARM64" (

@REM         @REM Check if PowerShell-7.2.24-win-x64.7z exists under ./downloads/tools, if so, extract with 7z located at ./downloads/tools/7zip-25.01-windows/7z.exe
@REM         if not exist "!VCPKG_PATH!\downloads\tools\PowerShell-7.2.24-win-x64.7z" (
@REM             echo [Download] PowerShell-7.2.24-win-x64.7z ...
@REM             "%VCPKG_EXE%" download powershell
@REM             if errorlevel 1 (
@REM                 echo [Error] PowerShell download failed
@REM                 exit /b 1
@REM             )
@REM         )

@REM         if not exist "!VCPKG_PATH!\downloads\tools\powershell-core-7.2.24-windows" (
@REM             echo [Extract] PowerShell-7.2.24-win-x64 ...
@REM             "!VCPKG_PATH!\downloads\tools\7zip-25.01-windows\7z.exe" x "!VCPKG_PATH!\downloads\tools\PowerShell-7.2.24-win-x64.7z" -o"!VCPKG_PATH!\downloads\tools" -y
@REM             if errorlevel 1 (
@REM                 echo [Error] PowerShell extraction failed
@REM                 exit /b 1
@REM             )

@REM             ren "!VCPKG_PATH!\downloads\tools\PowerShell-7.2.24-win-x64" "powershell-core-7.2.24-windows"
@REM             if errorlevel 1 (
@REM                 echo [Error] PowerShell rename failed
@REM                 exit /b 1
@REM             )
@REM         )
@REM     )
@REM ) else (
@REM     echo [Found] PowerShell already exists
@REM )



echo [Found] vcpkg path: %VCPKG_EXE%
echo [Run] vcpkg integrate install ...
"%VCPKG_EXE%" integrate install




@REM Install python externals
set "EXTERNALS_DIR=%PROJECT_ROOT%\kbe\src\lib\python\externals"
set "NUGET_EXE=%EXTERNALS_DIR%\nuget.exe"
set "CLONE_DIR=%~dp0\python-externals"
set "PYTHON_VERSION=3_13_5"

if exist "%NUGET_EXE%" (
    echo [INFO] nuget.exe already exists, skipping externals initialization.
) else (
    echo [INFO] nuget.exe not found, preparing to fetch externals...

    if exist "%EXTERNALS_DIR%" (
        echo [INFO] Deleting old externals folder...
        rmdir /s /q "%EXTERNALS_DIR%"
    )

    if exist "%CLONE_DIR%" (
        @REM Update existing repo
        git -C "%CLONE_DIR%" reset --hard HEAD
        git -C "%CLONE_DIR%" pull
    ) else (
        echo [INFO] Cloning python-externals from proxy...
        git clone https://gitcode.com/KBEngineLab/python-externals.git "%CLONE_DIR%"
        if errorlevel 1 (
            echo [ERROR] git clone failed!
            exit /b 1
        )
    )
    

    echo [INFO] Copying %PYTHON_VERSION% to externals...
    xcopy /e /i /y "%CLONE_DIR%\%PYTHON_VERSION%" "%EXTERNALS_DIR%"
    if errorlevel 1 (
        echo [ERROR] Copy externals failed!
        exit /b 1
    )

    echo [INFO] Removing temp directory python-externals...
    rmdir /s /q "%CLONE_DIR%"

    echo [INFO] Externals initialization completed.
)





REM =========================================
REM 2. Find VS installation path and all MSVC toolsets
REM =========================================
echo.
echo [Check] Searching for Visual Studio installation path...

set "VSWHERE_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE_PATH%" (
    echo [Error] vswhere.exe not found, please make sure Visual Studio or Build Tools is installed
    pause
    exit /b 1
)

set "VS_INSTALL_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE_PATH%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_INSTALL_PATH=%%i"
)

if not defined VS_INSTALL_PATH (
    echo [Error] Visual Studio with C++ toolset not found
    pause
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


REM List all MSVC toolsets
set "MSVC_COUNT=0"
set "VCVARS_VAR="
set "MSVC_VER_VAR="
for /d %%v in ("%MSVC_ROOT%\*") do (
    set /a MSVC_COUNT+=1
    set "MSVC_VER_!MSVC_COUNT!=%%~nxv"
)

echo.
if %MSVC_COUNT%==0 (
    echo [Warning] No MSVC toolset found, using default toolset
) else if %MSVC_COUNT%==1 (
    set "MSVC_VER=!MSVC_VER_1!"
    echo [Select] Using MSVC toolset version: !MSVC_VER!

    
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
    echo  MSVC latest version is: !LATEST_VER!

    set "MSVC_VER=!LATEST_VER!"
    echo [Select] Using MSVC toolset version: !MSVC_VER!


    set "MSVC_FULL_PATH=%MSVC_ROOT%\!MSVC_VER!"
    set "VCVARS_VAR=-vcvars_ver=!MSVC_VER!"
    set "MSVC_VER_VAR=-p:VCToolsVersion=!MSVC_VER!"
)


echo %MSVC_FULL_PATH%
echo %VCVARS_VAR%
echo %MSVC_VER_VAR%

echo VC_VER: %VC_VER%

set "VCVARSALL_BAT=%VS_INSTALL_PATH%\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARSALL_BAT%" x64 -vcvars_ver=%VC_VER%
if errorlevel 1 (
    echo [Error] Failed to load Visual Studio build environment
    pause
    exit /b 1
)




echo cl.exe path: %PATH%
cl



REM =========================================
REM 3. Build project
REM =========================================
echo.
echo KBEngine-Nex Build Script
echo Project Path: %PROJECT_ROOT%
echo Build Config: %CONFIG% ^| Platform: %PLATFORM%
echo Log File: %LOG_FILE%
echo.

echo [Step 1] Building KBEMain.vcxproj ...
msbuild "%INIT_BUILD_PROJ%" /p:Configuration=%CONFIG% %MSVC_VER_VAR% %PLATFORM_TOOLSET%  /p:Platform=%PLATFORM% /m /fileLogger /fileLoggerParameters:LogFile=%LOG_FILE%;Append;Encoding=UTF-8 /consoleloggerparameters:DisableConsoleColor 
if errorlevel 1 (
    echo [Error] KBEMain.vcxproj build failed, check %LOG_FILE%
    pause
    exit /b 1
)


if "%~3"=="GUICONSOLE" (
    echo [Install] Installing GUICONSOLE...
    goto GUICONSOLE
)


echo.
echo [Step 2] Building kbengine nex.sln ...
msbuild "%SOLUTION_FILE%" /p:Configuration=%CONFIG% %MSVC_VER_VAR% %PLATFORM_TOOLSET%  /p:Platform=Win64 /m  /fileLogger /fileLoggerParameters:LogFile=%LOG_FILE%;Append;Encoding=UTF-8 /consoleloggerparameters:DisableConsoleColor
if errorlevel 1 (
    echo [Error] kbengine nex.sln build failed, check %LOG_FILE%
    pause
    exit /b 1
)

echo.
echo [Success] Build completed!
pause
exit /b 0



:GUICONSOLE
echo.
echo [Step 2] Building GUICONSOLE
msbuild "%GUICONSOLE_SOLUTION_FILE%" /p:Configuration=%CONFIG% %MSVC_VER_VAR% %PLATFORM_TOOLSET%  /p:Platform=Win64 /m /fileLogger /fileLoggerParameters:LogFile=%LOG_FILE%;Append;Encoding=UTF-8 /consoleloggerparameters:DisableConsoleColor
if errorlevel 1 (
    echo [Error] guiconsole.sln build failed, check %LOG_FILE%
    pause
    exit /b 1
)

echo.
echo [Success] Build completed!
pause
exit /b 0

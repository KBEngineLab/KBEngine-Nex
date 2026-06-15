@echo off
chcp 65001 >nul
echo ============================================
echo   KBEngine Vcpkg Uninstall (Windows)
echo ============================================
echo.

set "vcpkg1=%LOCALAPPDATA%\kbe-vcpkg-gitcode"
set "vcpkg2=%LOCALAPPDATA%\kbe-vcpkg-gitee"
set "vcpkg3=%LOCALAPPDATA%\kbe-vcpkg"
set "vcpkg4=%LOCALAPPDATA%\vcpkg"

echo The following directories will be removed:
echo   %vcpkg1%
echo   %vcpkg2%
echo   %vcpkg3%
echo   %vcpkg4%
echo.

set /p confirm="Continue? [Y/N]: "
if /i not "%confirm%"=="Y" (
    echo Aborted.
    exit /b 0
)

echo.

for %%d in ("%vcpkg1%" "%vcpkg2%" "%vcpkg3%" "%vcpkg4%") do (
    if exist %%d (
        echo Removing %%d ...
        rmdir /s /q %%d
        if exist %%d (
            echo   [FAILED] %%d could not be removed.
        ) else (
            echo   [OK] %%d removed.
        )
    ) else (
        echo   [SKIP] %%d does not exist.
    )
)

echo.
echo Uninstall completed.
pause

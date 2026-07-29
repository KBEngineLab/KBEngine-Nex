@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

REM ====================================================
REM 1. Check available Python command
REM ====================================================
set PYTHON_CMD=

if not defined PYTHON_CMD (
    set /p PYTHON_CMD=Please enter your Python executable path manually （e.g. C:\Python313\python.exe）:
)

if not exist "%PYTHON_CMD%" (
    echo [ERROR] The specified Python path does not exist: "%PYTHON_CMD%"
    echo Please re-run this script with a valid Python installation.
    exit /b 1
)

echo Using Python command: %PYTHON_CMD%

REM ====================================================
REM 2. Create virtual environment
REM ====================================================
set VENV_DIR=.venv

if exist "%VENV_DIR%" (
    echo [INFO] Virtual environment already exists: %VENV_DIR%
) else (
    echo Creating virtual environment...
    %PYTHON_CMD% -m venv "%VENV_DIR%"
    if errorlevel 1 (
        echo [ERROR] Failed to create virtual environment.
        exit /b 1
    )
)

REM ====================================================
REM 3. Activate virtual environment
REM ====================================================
if exist "%VENV_DIR%\Scripts\activate.bat" (
    call "%VENV_DIR%\Scripts\activate.bat"
) else (
    echo [ERROR] Activation script not found: %VENV_DIR%\Scripts\activate.bat
    exit /b 1
)

REM ====================================================
REM 4. Install dependencies
REM ====================================================
if exist "requirements.txt" (
    echo Installing dependencies...
    python -m pip install --upgrade pip
    python -m pip install -r requirements.txt
) else (
    echo requirements.txt not found, skipping dependency installation.
)

REM ====================================================
REM 5. Run database migrations
REM ====================================================
echo Running database migrations...
python manage.py makemigrations
python manage.py migrate

REM ====================================================
REM 6. Create Django superuser
REM ====================================================
echo.
echo Starting Django superuser creation (interactive)
echo --------------------------------------------------
python manage.py createsuperuser
echo --------------------------------------------------

REM ====================================================
REM 7. Done
REM ====================================================
echo.
echo Django environment setup complete!
echo.
echo To activate the virtual environment:
echo    call %VENV_DIR%\Scripts\activate.bat
echo.
echo Start development server:
echo    uvicorn KBESettings.asgi:application --reload --reload-include "*.html"
echo.
echo Start production server:
echo    uvicorn KBESettings.asgi:application
echo.
echo Current Python version:
python --version
echo Current Django version:
python -m django --version

pause

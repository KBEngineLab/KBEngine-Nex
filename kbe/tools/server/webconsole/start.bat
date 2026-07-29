@echo off
setlocal
chcp 65001 >nul
title Run Django (Uvicorn)

REM ===============================================
REM 1. Activate virtual environment
REM ===============================================
if not exist ".venv\Scripts\activate.bat" (
    echo Virtual environment not found: .venv\Scripts\activate.bat
    echo Please create it first by running setup.bat
    exit /b 1
)

call ".venv\Scripts\activate.bat"

REM ===============================================
REM 2. Run Uvicorn server (auto reload for HTML)
REM ===============================================
uvicorn KBESettings.asgi:application --reload --reload-include "*.html"

endlocal

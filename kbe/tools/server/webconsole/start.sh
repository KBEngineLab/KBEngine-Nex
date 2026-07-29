#!/usr/bin/env bash
set -e  # 出错立即退出
source .venv/bin/activate
uvicorn KBESettings.asgi:application --reload --reload-include "*.html"
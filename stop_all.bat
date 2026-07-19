@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1
title IoT Health Monitor - Stop All

:: ================================================================
:: stop_all.bat — Hentikan semua service
:: ================================================================

echo.
echo  ==============================================================
echo   IoT Health Monitor — Menghentikan Service...
echo  ==============================================================
echo.

:: Hentikan Python server
echo  [1/2] Menghentikan Python Server...
taskkill /FI "WINDOWTITLE eq IoT Python Server*" /T /F >nul 2>&1
:: Juga kill proses uvicorn jika ada
taskkill /FI "IMAGENAME eq python.exe" /FI "WINDOWTITLE eq IoT*" /T /F >nul 2>&1
echo        [OK] Python Server dihentikan.

:: Hentikan Mosquitto portable (jangan sentuh service)
echo  [2/2] Menghentikan Mosquitto portable...
taskkill /FI "WINDOWTITLE eq Mosquitto MQTT*" /T /F >nul 2>&1
echo        [OK] Mosquitto dihentikan (jika portable).
echo        (Jika Mosquitto jalan sebagai Windows Service, gunakan: net stop mosquitto)

echo.
echo  ==============================================================
echo   Semua service dihentikan.
echo  ==============================================================
echo.
pause

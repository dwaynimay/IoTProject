@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1
title IoT Health Monitor - Server

:: ================================================================
:: start_all.bat — Jalankan Mosquitto + Python Server
:: ================================================================
:: Alur:
::   1. Deteksi IP LAN
::   2. Jalankan Mosquitto (portable atau service)
::   3. Jalankan Python Server (FastAPI + MQTT + Dashboard)
::   4. Buka browser otomatis
:: ================================================================

echo.
echo  ==============================================================
echo   IoT Health Monitor — Menjalankan Service...
echo  ==============================================================
echo.

:: ── Simpan direktori project ────────────────────────────────────
set "PROJECT_DIR=%~dp0"
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"

:: ── Cek apakah install sudah dijalankan ─────────────────────────
if not exist "%PROJECT_DIR%\server\.venv\Scripts\activate.bat" (
    echo  [ERROR] Virtual environment belum dibuat!
    echo          Jalankan dulu: install_all.bat
    echo.
    pause
    exit /b 1
)

:: ── Deteksi IP LAN ──────────────────────────────────────────────
echo  [1/3] Mendeteksi IP LAN...

for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command ^
    "(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {$_.IPAddress -notlike '127.*' -and $_.PrefixOrigin -eq 'Dhcp'} | Select-Object -First 1 -ExpandProperty IPAddress)"`) do set "LOCAL_IP=%%i"

if not defined LOCAL_IP (
    :: Fallback: coba ambil dari ipconfig
    for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /c:"IPv4 Address"') do (
        set "IP_RAW=%%a"
        for /f "tokens=*" %%b in ("!IP_RAW!") do set "LOCAL_IP=%%b"
        goto :ip_found
    )
    :ip_found
)

if not defined LOCAL_IP (
    echo        [WARN] IP LAN tidak terdeteksi, fallback ke localhost.
    set "LOCAL_IP=localhost"
)

echo        IP LAN: %LOCAL_IP%
echo.

:: ── Jalankan Mosquitto ──────────────────────────────────────────
echo  [2/3] Menghidupkan Mosquitto MQTT Broker...

set "MOSQUITTO_DIR=%PROJECT_DIR%\mosquitto"
set "MOSQUITTO_EXE=%MOSQUITTO_DIR%\mosquitto.exe"
set "MOSQUITTO_CONF=%MOSQUITTO_DIR%\mosquitto.conf"
set "MOSQUITTO_STARTED=0"

:: Cek apakah Mosquitto sudah jalan
tasklist /FI "IMAGENAME eq mosquitto.exe" 2>nul | findstr /I "mosquitto.exe" >nul
if !errorLevel! equ 0 (
    echo        [OK] Mosquitto sudah berjalan.
    set "MOSQUITTO_STARTED=1"
    goto :mosquitto_ready
)

:: Opsi 1: Portable Mosquitto (di folder project)
if exist "%MOSQUITTO_EXE%" (
    echo        Menjalankan Mosquitto portable...
    if exist "%MOSQUITTO_CONF%" (
        start "Mosquitto MQTT" /MIN "%MOSQUITTO_EXE%" -c "%MOSQUITTO_CONF%" -v
    ) else (
        start "Mosquitto MQTT" /MIN "%MOSQUITTO_EXE%" -p 1883 -v
    )
    set "MOSQUITTO_STARTED=1"
    timeout /t 2 /nobreak >nul
    echo        [OK] Mosquitto portable berjalan di port 1883.
    goto :mosquitto_ready
)

:: Opsi 2: System-wide Mosquitto
where mosquitto >nul 2>&1
if !errorLevel! equ 0 (
    echo        Menjalankan Mosquitto dari PATH...
    start "Mosquitto MQTT" /MIN mosquitto -p 1883 -v
    set "MOSQUITTO_STARTED=1"
    timeout /t 2 /nobreak >nul
    echo        [OK] Mosquitto berjalan di port 1883.
    goto :mosquitto_ready
)

:: Opsi 3: Windows Service
net start mosquitto >nul 2>&1
if !errorLevel! equ 0 (
    echo        [OK] Mosquitto service dimulai.
    set "MOSQUITTO_STARTED=1"
    goto :mosquitto_ready
)

:: Semua opsi gagal
echo        [WARN] Mosquitto tidak ditemukan!
echo               Server akan tetap dijalankan, tapi MQTT tidak akan
echo               berfungsi tanpa broker.
echo.
echo               Solusi:
echo                 1. Jalankan install_all.bat untuk setup Mosquitto
echo                 2. Atau install manual: winget install EclipseFoundation.Mosquitto
echo.

:mosquitto_ready
echo.

:: ── Jalankan Python Server ──────────────────────────────────────
echo  [3/3] Menjalankan Python Server...

:: Set environment variable untuk server
set "MQTT_BROKER=%LOCAL_IP%"

:: Buka server di jendela baru
start "IoT Python Server" cmd /k "cd /d %PROJECT_DIR% && call server\.venv\Scripts\activate.bat && python -m server"

echo        [OK] Server dimulai di jendela baru.
echo.

:: ── Ringkasan ───────────────────────────────────────────────────
echo  ==============================================================
echo   Semua service sedang berjalan!
echo  ==============================================================
echo.
echo   MQTT Broker : %LOCAL_IP%:1883
echo   Dashboard   : http://%LOCAL_IP%:8000
echo   Mosquitto   : %MOSQUITTO_STARTED% (1=OK, 0=GAGAL)
echo.
echo  ==============================================================
echo.

:: Tunggu server siap, lalu buka browser
echo  Menunggu server siap (5 detik)...
timeout /t 5 /nobreak >nul

echo  Membuka Dashboard di browser...
start "" "http://%LOCAL_IP%:8000"

echo.
echo  Tekan tombol apa saja untuk menutup jendela ini.
echo  (Server dan Mosquitto tetap berjalan di background)
echo.
pause

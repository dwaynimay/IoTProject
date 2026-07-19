@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1
title IoT Health Monitor - Installer

:: ================================================================
:: install_all.bat — One-Click Setup for IoT Health Monitor
:: ================================================================
:: Menangani:
::   1. Verifikasi Python 3.10-3.12
::   2. Membuat virtual environment
::   3. Install semua dependensi pip
::   4. Download & setup Mosquitto portable (jika belum ada)
::   5. Membuat file .env dari template
:: ================================================================

echo.
echo  ==============================================================
echo   IoT Health Monitor - One-Click Installer
echo  ==============================================================
echo.

:: ── Simpan direktori project ────────────────────────────────────
set "PROJECT_DIR=%~dp0"
:: Hapus trailing backslash
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"

:: ================================================================
:: STEP 1: Verifikasi Python
:: ================================================================
echo  [1/5] Memeriksa Python...

:: Coba python, python3, py launcher
set "PY_CMD="
where python >nul 2>&1 && set "PY_CMD=python"
if not defined PY_CMD (
    where python3 >nul 2>&1 && set "PY_CMD=python3"
)
if not defined PY_CMD (
    where py >nul 2>&1 && set "PY_CMD=py -3"
)

if not defined PY_CMD (
    echo.
    echo  [ERROR] Python tidak ditemukan!
    echo.
    echo  Cara install Python:
    echo    1. Buka https://www.python.org/downloads/
    echo    2. Download Python 3.10, 3.11, atau 3.12
    echo    3. PENTING: Centang "Add Python to PATH" saat install
    echo    4. Jalankan ulang install_all.bat
    echo.
    pause
    exit /b 1
)

:: Cek versi Python
for /f "tokens=2 delims= " %%v in ('%PY_CMD% --version 2^>^&1') do set "PY_VER=%%v"
echo        Python ditemukan: %PY_VER% (%PY_CMD%)

:: Parse major.minor
for /f "tokens=1,2 delims=." %%a in ("%PY_VER%") do (
    set "PY_MAJOR=%%a"
    set "PY_MINOR=%%b"
)

:: Validasi versi: harus 3.10-3.12
set "PY_OK=0"
if "%PY_MAJOR%"=="3" (
    if "%PY_MINOR%"=="10" set "PY_OK=1"
    if "%PY_MINOR%"=="11" set "PY_OK=1"
    if "%PY_MINOR%"=="12" set "PY_OK=1"
)

if "%PY_OK%"=="0" (
    echo.
    echo  [ERROR] Versi Python %PY_VER% tidak didukung.
    echo          Project ini membutuhkan Python 3.10, 3.11, atau 3.12.
    echo          Download di: https://www.python.org/downloads/
    echo.
    pause
    exit /b 1
)
echo        [OK] Versi Python kompatibel.
echo.

:: ================================================================
:: STEP 2: Membuat Virtual Environment
:: ================================================================
echo  [2/5] Membuat Virtual Environment...

if exist "%PROJECT_DIR%\server\.venv\Scripts\python.exe" (
    echo        [SKIP] .venv sudah ada, melewati pembuatan.
) else (
    %PY_CMD% -m venv "%PROJECT_DIR%\server\.venv"
    if !errorLevel! neq 0 (
        echo  [ERROR] Gagal membuat virtual environment.
        echo          Pastikan modul 'venv' tersedia:
        echo            pip install virtualenv
        pause
        exit /b 1
    )
    echo        [OK] Virtual environment dibuat di server\.venv\
)
echo.

:: ================================================================
:: STEP 3: Install Dependensi Python
:: ================================================================
echo  [3/5] Menginstal dependensi Python...

call "%PROJECT_DIR%\server\.venv\Scripts\activate.bat"

:: Upgrade pip dulu (suppress output)
python -m pip install --upgrade pip >nul 2>&1

:: Install requirements
pip install -r "%PROJECT_DIR%\server\requirements.txt"
if !errorLevel! neq 0 (
    echo.
    echo  [ERROR] Gagal menginstal dependensi.
    echo          Periksa koneksi internet Anda.
    pause
    exit /b 1
)
echo        [OK] Semua dependensi Python terinstal.
echo.

:: ================================================================
:: STEP 4: Setup Mosquitto MQTT Broker
:: ================================================================
echo  [4/5] Memeriksa Mosquitto MQTT Broker...

set "MOSQUITTO_DIR=%PROJECT_DIR%\mosquitto"
set "MOSQUITTO_EXE=%MOSQUITTO_DIR%\mosquitto.exe"
set "MOSQUITTO_CONF=%MOSQUITTO_DIR%\mosquitto.conf"

:: Cek apakah Mosquitto sudah terinstal secara system-wide
where mosquitto >nul 2>&1
if !errorLevel! equ 0 (
    echo        [OK] Mosquitto sudah terinstal di sistem.
    goto :mosquitto_done
)

:: Cek apakah portable Mosquitto sudah ada
if exist "%MOSQUITTO_EXE%" (
    echo        [OK] Mosquitto portable sudah ada.
    goto :mosquitto_conf
)

:: Download Mosquitto portable
echo        Mosquitto belum ditemukan. Mendownload versi portable...
echo.

:: Buat folder mosquitto
if not exist "%MOSQUITTO_DIR%" mkdir "%MOSQUITTO_DIR%"

:: Download menggunakan PowerShell
set "MOSQUITTO_URL=https://mosquitto.org/files/binary/win64/mosquitto-2.0.21-install-windows-x64.exe"
set "MOSQUITTO_INSTALLER=%MOSQUITTO_DIR%\mosquitto_installer.exe"

echo        Mendownload dari mosquitto.org...
powershell -NoProfile -Command ^
    "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; " ^
    "try { " ^
    "  Invoke-WebRequest -Uri '%MOSQUITTO_URL%' -OutFile '%MOSQUITTO_INSTALLER%' -UseBasicParsing; " ^
    "  Write-Host '        [OK] Download selesai.' " ^
    "} catch { " ^
    "  Write-Host '        [ERROR] Gagal download Mosquitto.' -ForegroundColor Red; " ^
    "  Write-Host '        Silakan download manual:'; " ^
    "  Write-Host '          1. Buka https://mosquitto.org/download/'; " ^
    "  Write-Host '          2. Download Windows 64-bit installer'; " ^
    "  Write-Host '          3. Install ke folder: %MOSQUITTO_DIR%'; " ^
    "  exit 1 " ^
    "}"

if !errorLevel! neq 0 (
    echo.
    echo  [WARN] Download otomatis gagal.
    echo         Anda bisa install Mosquitto secara manual:
    echo           1. Buka https://mosquitto.org/download/
    echo           2. Download "mosquitto-2.0.21-install-windows-x64.exe"
    echo           3. Install dengan pilih folder: %MOSQUITTO_DIR%
    echo         Atau install via Windows Service ^(Run as Admin^):
    echo           winget install EclipseFoundation.Mosquitto
    echo.
    echo         Lanjut tanpa Mosquitto? Server tetap bisa jalan jika
    echo         Mosquitto sudah terinstal di tempat lain.
    echo.
    pause
    goto :mosquitto_done
)

:: Extract menggunakan 7z atau manual install notice
echo.
echo  ╔══════════════════════════════════════════════════════════╗
echo  ║  Installer Mosquitto sudah didownload.                  ║
echo  ║                                                         ║
echo  ║  Silakan jalankan installer tersebut:                   ║
echo  ║    %MOSQUITTO_INSTALLER%
echo  ║                                                         ║
echo  ║  PENTING: Pilih Install Location ke:                    ║
echo  ║    %MOSQUITTO_DIR%
echo  ║                                                         ║
echo  ║  Setelah install selesai, tekan Enter untuk lanjut.     ║
echo  ╚══════════════════════════════════════════════════════════╝
echo.

:: Buka installer otomatis
start "" "%MOSQUITTO_INSTALLER%"
echo  Menunggu Anda selesai menginstal Mosquitto...
pause

:: Verifikasi
if not exist "%MOSQUITTO_EXE%" (
    echo  [WARN] mosquitto.exe tidak ditemukan di %MOSQUITTO_DIR%
    echo         Pastikan Anda menginstal ke folder yang benar.
    echo         Server tetap bisa jalan jika Mosquitto sudah ada di PATH.
)

:mosquitto_conf
:: Buat konfigurasi Mosquitto yang aman
if not exist "%MOSQUITTO_CONF%" (
    echo        Membuat mosquitto.conf...
    (
        echo # ================================================================
        echo # Mosquitto Config untuk IoT Health Monitor
        echo # ================================================================
        echo.
        echo # Listener — menerima koneksi dari semua IP di port 1883
        echo listener 1883 0.0.0.0
        echo.
        echo # Izinkan koneksi tanpa autentikasi ^(untuk development^)
        echo allow_anonymous true
        echo.
        echo # Logging
        echo log_dest stderr
        echo log_type warning
        echo log_type error
        echo.
        echo # Persistence
        echo persistence false
    ) > "%MOSQUITTO_CONF%"
    echo        [OK] mosquitto.conf dibuat.
) else (
    echo        [OK] mosquitto.conf sudah ada.
)

:mosquitto_done
echo.

:: ================================================================
:: STEP 5: Setup file .env
:: ================================================================
echo  [5/5] Memeriksa konfigurasi .env...

if exist "%PROJECT_DIR%\server\.env" (
    echo        [OK] server\.env sudah ada, tidak diubah.
) else (
    if exist "%PROJECT_DIR%\server\.env.example" (
        copy "%PROJECT_DIR%\server\.env.example" "%PROJECT_DIR%\server\.env" >nul
        echo        [OK] server\.env dibuat dari template.
    ) else (
        :: Buat .env minimal
        (
            echo MQTT_BROKER=localhost
            echo MQTT_PORT=1883
            echo MQTT_KEEPALIVE=60
            echo TOPIC_BASE=health_monitor
            echo CS_ALGORITHM=hadamard
            echo LOG_LEVEL=INFO
            echo DB_PATH=server/data/health_monitor.db
            echo RETENTION_HOURS=24
            echo TS_SPREAD_TOLERANCE_MS=500
        ) > "%PROJECT_DIR%\server\.env"
        echo        [OK] server\.env dibuat dengan nilai default.
    )
)
echo.

:: ================================================================
:: SELESAI
:: ================================================================
echo  ==============================================================
echo   INSTALASI SELESAI!
echo  ==============================================================
echo.
echo   Ringkasan:
echo     Python    : %PY_VER% (%PY_CMD%)
echo     Venv      : server\.venv\
echo     Mosquitto : %MOSQUITTO_DIR%\
echo     Config    : server\.env
echo.
echo   Untuk menjalankan project:
echo     Klik dua kali: start_all.bat
echo.
echo   Untuk firmware (PlatformIO):
echo     1. Buka folder firmware/ di VS Code
echo     2. PlatformIO akan otomatis mendeteksi platformio.ini
echo     3. Build dan upload ke ESP32
echo.
echo  ==============================================================
echo.
pause

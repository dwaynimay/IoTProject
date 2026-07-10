@echo off
:: ================================================================
:: start_all.bat — Jalankan Mosquitto dan Server Python
:: ================================================================

echo ================================================================
echo  IoT Server — Menjalankan Service...
echo ================================================================
echo.

:: ── Deteksi IP LAN (ambil adapter DHCP pertama, skip 127.x) ────
for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command ^
    "(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {$_.IPAddress -notlike '127.*' -and $_.PrefixOrigin -eq 'Dhcp'} | Select-Object -First 1 -ExpandProperty IPAddress)"`) do set LOCAL_IP=%%i

if not defined LOCAL_IP (
    echo [WARN] IP LAN tidak terdeteksi, fallback ke localhost.
    set LOCAL_IP=localhost
)

echo IP LAN terdeteksi: %LOCAL_IP%
echo.

:: 1. Mosquitto MQTT Broker
echo [1/2] Menghidupkan Mosquitto MQTT Broker...
net start mosquitto > nul 2>&1
if %errorLevel% == 0 (
    echo      [OK] Mosquitto service berjalan.
) else (
    echo      [INFO] Perintah 'net start mosquitto' gagal (mungkin karena butuh Run as Administrator, atau belum diinstal sebagai service). 
    echo             Jika Mosquitto sudah jalan di background, abaikan pesan ini.
)

:: Cek apakah venv sudah diinstall
if not exist "%~dp0server\.venv\Scripts\activate.bat" (
    echo [ERROR] Virtual environment tidak ditemukan di server\.venv\
    echo         Jalankan dulu: install_all.bat
    pause
    exit /b 1
)

:: 2. Backend Python — buka di jendela baru
echo [2/2] Membuka Python Server (FastAPI + MQTT + Dashboard)...
start "IoT Python Server" cmd /k "call %~dp0server\.venv\Scripts\activate.bat && cd /d %~dp0 && python -m server"

echo.
echo ================================================================
echo  Semua service sedang dimulai!
echo  - MQTT Broker: %LOCAL_IP%:1883
echo  - Dashboard  : http://%LOCAL_IP%:8000
echo ================================================================
echo.

:: Tunggu server siap lalu buka browser
timeout /t 5 /nobreak > nul
echo Membuka Dashboard di browser...
start "" http://%LOCAL_IP%:8000

pause

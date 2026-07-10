@echo off
echo ================================================================
echo  IoT Server - Menginstal Dependensi Python
echo ================================================================
echo.

:: Pastikan Python tersedia
python --version >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERROR] Python tidak ditemukan. Pastikan Python sudah terinstal dan ada di PATH.
    pause
    exit /b 1
)

echo [1/2] Membuat Virtual Environment Python di server\.venv ...
python -m venv server\.venv
if %errorLevel% neq 0 (
    echo [ERROR] Gagal membuat virtual environment.
    pause
    exit /b %errorLevel%
)
echo [OK] Virtual Environment berhasil dibuat.
echo.

echo [2/2] Menginstal semua dependensi dari server\requirements.txt ...
call server\.venv\Scripts\activate.bat
pip install --upgrade pip >nul
pip install -r server\requirements.txt
if %errorLevel% neq 0 (
    echo [ERROR] Gagal menginstal dependensi.
    pause
    exit /b %errorLevel%
)
echo [OK] Semua dependensi selesai diinstal.
echo.

echo ================================================================
echo  Semua dependensi berhasil diinstal!
echo  Anda sekarang bisa menjalankan proyek dengan mengklik dua kali:
echo  1. start_all.bat
echo ================================================================
pause

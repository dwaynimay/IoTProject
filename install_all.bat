@echo off
echo ================================================================
echo  IoT Server - Menginstal Dependensi Python
echo ================================================================
echo.

echo [1/2] Membuat Virtual Environment Python (.venv)...
python -m venv .venv
if %errorLevel% neq 0 (
    echo [ERROR] Gagal membuat virtual environment. Pastikan Python terinstal dan ada di PATH.
    pause
    exit /b %errorLevel%
)
echo [OK] Virtual Environment berhasil dibuat.
echo.

echo [2/2] Menginstal dependensi dari requirements.txt...
call .venv\Scripts\activate.bat
cd server
pip install -r requirements.txt
cd ..
echo [OK] Dependensi Python selesai diinstal.
echo.

echo ================================================================
echo  Semua dependensi berhasil diinstal!
echo  Anda sekarang bisa menjalankan proyek dengan mengklik dua kali:
echo  1. start_all.bat
echo ================================================================
pause

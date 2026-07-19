# IoT Health Monitor - Setup & Deployment

## Cara Setup di Komputer Baru (One-Click)

### Prasyarat
- **Windows 10/11** (64-bit)
- **Python 3.10, 3.11, atau 3.12** — [Download di sini](https://www.python.org/downloads/)
  - ⚠️ **PENTING**: Centang **"Add Python to PATH"** saat install!
- **Koneksi internet** (untuk download dependensi pertama kali)

### Langkah Install

```
1. Clone atau copy folder IoTProject ke komputer tujuan
2. Klik dua kali: install_all.bat
3. Tunggu sampai selesai (~2-5 menit)
4. Selesai!
```

### Langkah Menjalankan

```
1. Klik dua kali: start_all.bat
2. Browser akan terbuka otomatis ke Dashboard
3. Untuk menghentikan: klik dua kali stop_all.bat
```

### Apa yang dilakukan `install_all.bat`?

| Step | Apa yang terjadi |
|------|-----------------|
| 1 | Cek Python 3.10-3.12 tersedia di PATH |
| 2 | Buat virtual environment di `server\.venv\` |
| 3 | Install semua library Python (FastAPI, numpy, dll) |
| 4 | Download & setup Mosquitto MQTT Broker portable |
| 5 | Buat file konfigurasi `server\.env` |

### Apa yang dilakukan `start_all.bat`?

| Step | Apa yang terjadi |
|------|-----------------|
| 1 | Deteksi IP LAN otomatis |
| 2 | Jalankan Mosquitto (portable → PATH → service) |
| 3 | Jalankan Python Server (FastAPI + MQTT + Dashboard) |
| 4 | Buka browser ke Dashboard |

### Troubleshooting

| Masalah | Solusi |
|---------|--------|
| "Python tidak ditemukan" | Install Python 3.10+ dan centang "Add to PATH" |
| "Gagal install dependensi" | Cek koneksi internet, coba `pip install --upgrade pip` |
| "Mosquitto tidak ditemukan" | Jalankan ulang `install_all.bat` atau install manual via `winget install EclipseFoundation.Mosquitto` |
| Dashboard tidak terbuka | Cek apakah port 8000 sudah dipakai: `netstat -an | findstr 8000` |
| MQTT tidak konek | Cek apakah Mosquitto jalan: `tasklist | findstr mosquitto` |

### Struktur File

```
IoTProject/
├── install_all.bat    ← Klik pertama kali (setup)
├── start_all.bat      ← Klik untuk menjalankan
├── stop_all.bat       ← Klik untuk menghentikan
├── mosquitto/         ← Mosquitto portable (dibuat oleh installer)
│   ├── mosquitto.exe
│   └── mosquitto.conf
├── server/            ← Backend Python
│   ├── .venv/         ← Virtual environment (dibuat oleh installer)
│   ├── .env           ← Konfigurasi (dibuat oleh installer)
│   └── requirements.txt
└── firmware/          ← ESP32 firmware (buka di PlatformIO)
```

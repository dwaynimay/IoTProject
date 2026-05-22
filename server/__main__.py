"""
server/__main__.py

Entry point tunggal untuk seluruh sistem server.

Jalankan dari root project:
    python -m server

Menjalankan dalam SATU proses:
    - FastAPI REST API + WebSocket dashboard  (port 8000)
    - MQTT worker thread (subscribe + rekonstruksi CS)
    - SQLite storage
    - WebSocket push real-time ke semua client

Untuk debugging komponen/aplikasi individual:
    python -m server.apps.reconstruct    # hanya MQTT + rekonstruksi
    python -m server.apps.dashboard      # hanya dashboard

Development/debug tools (bukan bagian server production):
    python -m server.tools.live_visualizer    # visualisasi matplotlib real-time
    python -m server.tools.test_single_signal # test rekonstruksi satu sinyal
    python -m server.tools.verify_phi         # verifikasi matriks Φ vs firmware
"""

import sys
import os

# Pastikan server/ ada di sys.path saat dijalankan dari root
sys.path.insert(0, os.path.dirname(__file__))

from apps.main_app import main

if __name__ == "__main__":
    main()

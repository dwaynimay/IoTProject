"""
server/__main__.py

Dijalankan saat seseorang mengetik: python -m server
Menampilkan petunjuk cara menjalankan masing-masing app.
"""

print("""
╔══════════════════════════════════════════════════════╗
║   ESP32 Health Monitor — Server                      ║
╚══════════════════════════════════════════════════════╝

Cara menjalankan (dari root folder project):

  python -m server.apps.reconstruct_server
      → Subscribe MQTT, rekonstruksi semua sinyal CS, print hasil.

  python -m server.apps.live_visualizer
      → Visualisasi real-time (matplotlib). Jalankan bersamaan
        dengan reconstruct_server di terminal terpisah.

  python -m server.apps.test_single_signal
      → Test rekonstruksi 1 sinyal (default: gx).
        Berguna untuk tuning LASSO_ALPHA.

Konfigurasi:
  Edit server/core/config.py → ubah MQTT_BROKER, CS_M, LASSO_ALPHA, dll.

Install dependencies (sekali saja):
  pip install -r server/requirements.txt
""")

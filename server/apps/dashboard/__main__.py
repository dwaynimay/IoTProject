# File: server/apps/dashboard/__main__.py
#
# Jalankan: python -m server.apps.dashboard  (dari root project)
#       atau python -m apps.dashboard         (dari folder server/)

import os
import sys

# Pastikan folder server/ ada di sys.path agar import 'core', 'apps', dll bisa resolve
_SERVER_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

import uvicorn


def main():
    uvicorn.run(
        "apps.dashboard.app:app",
        host    = "0.0.0.0",
        port    = 8000,
        reload  = False,
        workers = 1,   # WAJIB 1 agar hub & storage tidak ter-fork
    )


if __name__ == "__main__":
    main()

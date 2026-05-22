# File: server/apps/dashboard/__main__.py

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

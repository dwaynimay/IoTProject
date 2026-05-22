# File: server/apps/__main__.py

import sys


def main():
    print("=" * 60)
    print("  ESP32 Health Monitor — Server Application Launcher")
    print("=" * 60)
    print("Available applications / modules to run:")
    print("  1. Combined Server (FastAPI Dashboard + MQTT Reconstructor)")
    print("     Command: python -m server")
    print("     File   : server/apps/main_app.py")
    print()
    print("  2. Reconstruct Server (MQTT Reconstructor loop only)")
    print("     Command: python -m server.apps.reconstruct")
    print("     File   : server/apps/reconstruct/__main__.py")
    print()
    print("  3. Dashboard Server (FastAPI REST & WebSockets only)")
    print("     Command: python -m server.apps.dashboard")
    print("     File   : server/apps/dashboard/__main__.py")
    print("=" * 60)
    print("To start one, run the command shown above or choose:")
    print("  [1] Start Combined Server")
    print("  [2] Start Reconstruct Server")
    print("  [3] Start Dashboard Server")
    print("  [q] Quit")
    print()

    try:
        choice = input("Enter choice: ").strip().lower()
    except (KeyboardInterrupt, EOFError):
        print("\nExit.")
        return

    if choice == '1':
        from apps.main_app import main as run_combined
        run_combined()
    elif choice == '2':
        from apps.reconstruct.__main__ import main as run_rec
        run_rec()
    elif choice == '3':
        from apps.dashboard.__main__ import main as run_dash
        run_dash()
    else:
        print("Exit.")


if __name__ == "__main__":
    main()

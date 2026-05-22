# Health Monitor Server

This is the backend server for the IoT Health Monitor project. It handles MQTT messages from ESP32 nodes, reconstructs compressive sensing signals, assesses signal quality, stores data in SQLite, and serves a live dashboard via FastAPI and WebSockets.

## Architecture

- **`core/`**: Pure logic (validation, quality assessment, SQLite storage). No web or orchestration logic.
- **`cs/`**: Compressive Sensing algorithms (OMP, LASSO). Isolated library.
- **`apps/`**: Applications and services.
  - **`reconstruct/`**: Orchestrator (Listener → Validate → Reconstruct → Quality → Storage → Notifier).
  - **`dashboard/`**: FastAPI REST API and WebSocket hub.
  - **`ml_inference/`**: ML model inference (Stub for now).
- **`tools/`**: CLI utilities and debugging scripts.
- **`tests/`**: Unit and integration tests.
- **`static/`**: Frontend assets for the dashboard.

## Setup

1. Create a Python virtual environment:
   ```bash
   python -m venv .venv
   ```

2. Activate the virtual environment:
   - Windows: `.venv\Scripts\activate`
   - Linux/Mac: `source .venv/bin/activate`

3. Install dependencies:
   ```bash
   pip install -r requirements.txt
   pip install -r requirements-dev.txt
   ```

4. Configure environment variables:
   ```bash
   cp .env.example .env
   # Edit .env as needed
   ```

## Running the Server

Run the combined reconstructor and dashboard server:
```bash
python -m server
```

Or run individual services:
```bash
python -m server.apps.reconstruct
python -m server.apps.dashboard
```

## Testing

Run the test suite:
```bash
pytest tests/
```

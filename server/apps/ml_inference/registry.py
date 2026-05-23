"""
registry.py — ModelRegistry: mengelola banyak model secara dinamis.

Filosofi:
    Registry adalah "folder watcher + engine pool".
    Tambah model baru = taruh .pkl + _config.json di models/ → scan/register → selesai.
    Semua model jalan parallel per window, hasilnya dikumpulkan di MultiModelResult.

    Label tiap model sepenuhnya dikontrol dari config JSON masing-masing —
    tidak ada label yang hardcode di registry maupun engine.
    Dashboard cukup baca to_dict() dan render apapun yang ada di sana.

Cara pakai:

    # Setup sekali di startup:
    registry = ModelRegistry()
    registry.scan("server/apps/ml_inference/models/")   # auto-detect semua pasangan

    # Atau register manual:
    registry.register("models/fall_detector.pkl", "models/fall_detector_config.json")

    # Per window (real-time):
    mmr = registry.predict_all(window_input)
    print(mmr.summary_str())
    ws.send(json.dumps(mmr.to_dict()))   # langsung ke dashboard

    # Batch (dari SQLite):
    mmr_list = registry.predict_all_batch(window_inputs)

    # Hot-reload model tertentu tanpa restart:
    registry.reload("fall_detector")

    # Tambah model baru saat runtime:
    registry.register("models/stress_monitor.pkl", "models/stress_monitor_config.json")

    # Nonaktifkan sementara (tetap di memori, skip saat predict):
    registry.disable("fall_detector")
    registry.enable("fall_detector")

    # Lihat status semua model:
    print(registry.status())   # untuk REST /api/ml/status

Konvensi naming file (untuk scan()):
    model file : <model_name>.pkl
    config file: <model_name>_config.json   ← prioritas utama
                 <model_name>.json          ← fallback

    Contoh:
        models/activity_classifier.pkl
        models/activity_classifier_config.json
        models/fall_detector.pkl
        models/fall_detector_config.json
"""

from __future__ import annotations

import logging
import threading
import time
from pathlib import Path
from typing import Optional

from .engine  import MLInferenceEngine
from .schemas import InferenceResult, MultiModelResult, WindowInput

logger = logging.getLogger(__name__)


# ── Entry internal registry ───────────────────────────────────────────────────

class _ModelEntry:
    """Satu slot di registry: engine + path + status."""

    def __init__(self, model_name: str, model_path: Path, config_path: Path) -> None:
        self.model_name  = model_name
        self.model_path  = model_path
        self.config_path = config_path
        self.engine      = MLInferenceEngine()
        self.enabled     = True
        self.load_error: Optional[str] = None
        self.loaded_at:  Optional[float] = None

    def load(self) -> None:
        try:
            self.engine.load(self.model_path, self.config_path)
            self.load_error = None
            self.loaded_at  = time.time()
            logger.info("Registry: loaded '%s'", self.model_name)
        except Exception as exc:
            self.load_error = str(exc)
            logger.error("Registry: GAGAL load '%s': %s", self.model_name, exc)

    def is_ready(self) -> bool:
        return self.enabled and self.engine.is_loaded and self.load_error is None

    def info(self) -> dict:
        return {
            "model_name":  self.model_name,
            "model_path":  str(self.model_path),
            "config_path": str(self.config_path),
            "enabled":     self.enabled,
            "loaded":      self.engine.is_loaded,
            "load_error":  self.load_error,
            "loaded_at":   self.loaded_at,
            "version":     self.engine.model_version if self.engine.is_loaded else None,
            # Labels diambil dari engine, sepenuhnya dikontrol oleh config JSON
            "labels":      self.engine.labels if self.engine.is_loaded else [],
            "n_features":  self.engine.stats().get("n_features", 0),
        }


# ── Registry ──────────────────────────────────────────────────────────────────

class ModelRegistry:
    """
    Pool dinamis MLInferenceEngine yang jalan parallel.

    Jumlah model tidak dibatasi — tambah/hapus kapanpun tanpa restart.
    Label tiap model berasal dari config masing-masing, sehingga tiap model
    bisa prediksi hal yang sama sekali berbeda (aktivitas, fall, stress, dll).

    Thread-safe: semua mutasi (register/reload/enable/disable) menggunakan _lock.
    predict_all() hanya baca snapshot → tidak blocking inferensi yang sedang jalan.
    """

    def __init__(self) -> None:
        self._entries: dict[str, _ModelEntry] = {}
        self._lock    = threading.Lock()

    # ── Registration ─────────────────────────────────────────────────────────

    def register(
        self,
        model_path:  "str | Path",
        config_path: "str | Path",
        *,
        model_name: Optional[str] = None,
        auto_load:  bool = True,
    ) -> str:
        """
        Daftarkan satu model ke registry.

        Args:
            model_path  : path ke .pkl
            config_path : path ke _config.json
            model_name  : override nama (default: stem dari model_path)
            auto_load   : langsung load model (default True)

        Returns:
            model_name yang terdaftar
        """
        model_path  = Path(model_path)
        config_path = Path(config_path)
        name        = model_name or model_path.stem

        entry = _ModelEntry(name, model_path, config_path)

        with self._lock:
            if name in self._entries:
                logger.warning("Registry: '%s' sudah ada, menimpa.", name)
            self._entries[name] = entry

        if auto_load:
            entry.load()

        return name

    def scan(
        self,
        models_dir: "str | Path",
        *,
        auto_load: bool = True,
        recursive: bool = False,
    ) -> list[str]:
        """
        Scan folder dan auto-register semua pasangan .pkl + config.

        Konvensi penamaan:
            <name>.pkl  +  <name>_config.json   <- prioritas utama
            <name>.pkl  +  <name>.json           <- fallback

        Args:
            models_dir : folder yang di-scan
            auto_load  : langsung load semua model yang ditemukan
            recursive  : jika True, scan semua subfolder secara rekursif.
                         Berguna untuk struktur models/imu/, models/ppg/, dll.
                         Nama model = stem dari .pkl (tanpa prefix subfolder).

        Returns:
            list model_name yang berhasil didaftarkan
        """
        models_dir = Path(models_dir)
        if not models_dir.exists():
            logger.warning("Registry scan: folder '%s' tidak ada", models_dir)
            return []

        registered: list[str] = []
        glob_pattern = "**/*.pkl" if recursive else "*.pkl"

        for pkl_path in sorted(models_dir.glob(glob_pattern)):
            name = pkl_path.stem
            # Config dicari di folder yang sama dengan pkl
            pkl_dir = pkl_path.parent

            config_candidates = [
                pkl_dir / f"{name}_config.json",
                pkl_dir / f"{name}.json",
            ]
            config_path = next((p for p in config_candidates if p.exists()), None)

            if config_path is None:
                logger.warning(
                    "Registry scan: '%s' tidak punya config, skip. "
                    "Buat '%s_config.json' di folder yang sama.",
                    pkl_path.name, name,
                )
                continue

            self.register(
                model_path  = pkl_path,
                config_path = config_path,
                model_name  = name,
                auto_load   = auto_load,
            )
            registered.append(name)

        logger.info("Registry scan '%s' (recursive=%s): %d model ditemukan -> %s",
                    models_dir, recursive, len(registered), registered)
        return registered

    def unregister(self, model_name: str) -> bool:
        """Hapus model dari registry dan bebaskan memori."""
        with self._lock:
            entry = self._entries.pop(model_name, None)
        if entry is None:
            logger.warning("Registry unregister: '%s' tidak ditemukan", model_name)
            return False
        entry.engine.unload()
        logger.info("Registry: '%s' dihapus", model_name)
        return True

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    def reload(self, model_name: str) -> bool:
        """
        Hot-reload model tertentu tanpa menyentuh model lain.
        Berguna saat .pkl diperbarui (retrain) tanpa restart server.
        Config juga di-reload — label baru otomatis aktif.
        """
        with self._lock:
            entry = self._entries.get(model_name)
        if entry is None:
            logger.error("Registry reload: '%s' tidak ditemukan", model_name)
            return False
        entry.load()
        return entry.load_error is None

    def reload_all(self) -> dict[str, bool]:
        """Reload semua model. Return dict model_name → sukses."""
        with self._lock:
            names = list(self._entries.keys())
        return {name: self.reload(name) for name in names}

    def enable(self, model_name: str) -> None:
        """Aktifkan model. Model yang disabled di-skip saat predict."""
        with self._lock:
            if model_name in self._entries:
                self._entries[model_name].enabled = True
                logger.info("Registry: '%s' enabled", model_name)

    def disable(self, model_name: str) -> None:
        """Nonaktifkan model sementara tanpa unload dari memori."""
        with self._lock:
            if model_name in self._entries:
                self._entries[model_name].enabled = False
                logger.info("Registry: '%s' disabled", model_name)

    # ── Predict ───────────────────────────────────────────────────────────────

    def predict_all(self, window: WindowInput) -> MultiModelResult:
        """
        Jalankan semua model aktif untuk satu window.

        Setiap model menghasilkan InferenceResult dengan label dari config-nya
        masing-masing — tidak ada asumsi label di sini.

        Model yang disabled atau gagal load menghasilkan InferenceResult
        dengan skipped=True (tetap masuk MultiModelResult, skip_reason tercatat).

        Thread-safe: snapshot entries sebelum inferensi dimulai.
        """
        with self._lock:
            entries = list(self._entries.values())

        mmr = MultiModelResult(
            node_id    = window.node_id,
            window_num = window.window_num,
            ts         = window.ts,
        )

        t0 = time.perf_counter()

        for entry in entries:
            if not entry.enabled:
                result = InferenceResult(
                    node_id=window.node_id, window_num=window.window_num, ts=window.ts,
                    model_name=entry.model_name,
                    skipped=True, skip_reason="model disabled",
                )
            elif not entry.engine.is_loaded:
                result = InferenceResult(
                    node_id=window.node_id, window_num=window.window_num, ts=window.ts,
                    model_name=entry.model_name,
                    skipped=True, skip_reason=f"model not loaded: {entry.load_error or ''}",
                )
            else:
                result = entry.engine.predict(window)
                result.model_name = entry.model_name  # pastikan ter-set dari registry

            mmr.add(result)

        mmr.total_ms = (time.perf_counter() - t0) * 1000
        logger.debug("predict_all win=%d | %s | %.1fms",
                     window.window_num, mmr.summary_str(), mmr.total_ms)
        return mmr

    def predict_all_batch(
        self,
        windows: list[WindowInput],
    ) -> list[MultiModelResult]:
        """
        Batch predict: tiap model jalan batch-nya sendiri, lalu di-zip ke MultiModelResult.

        Lebih efisien untuk data dari SQLite dibanding loop predict_all per window.

        Returns:
            list MultiModelResult, panjang sama dengan windows.
            Urutan dijaga: mmr_list[i] sesuai dengan windows[i].
        """
        if not windows:
            return []

        with self._lock:
            entries = list(self._entries.values())

        # Hasil per model: model_name → list[InferenceResult] (len = len(windows))
        model_results: dict[str, list[InferenceResult]] = {}

        for entry in entries:
            if not entry.enabled or not entry.engine.is_loaded:
                skip_reason = ("model disabled" if not entry.enabled
                               else f"model not loaded: {entry.load_error or ''}")
                model_results[entry.model_name] = [
                    InferenceResult(
                        node_id=w.node_id, window_num=w.window_num, ts=w.ts,
                        model_name=entry.model_name,
                        skipped=True, skip_reason=skip_reason,
                    )
                    for w in windows
                ]
            else:
                batch = entry.engine.predict_batch(windows)
                for r in batch:
                    r.model_name = entry.model_name
                model_results[entry.model_name] = batch

        # Zip ke MultiModelResult per window
        mmr_list: list[MultiModelResult] = []
        for i, window in enumerate(windows):
            mmr = MultiModelResult(
                node_id=window.node_id, window_num=window.window_num, ts=window.ts,
            )
            for model_name, results in model_results.items():
                mmr.add(results[i])
            mmr_list.append(mmr)

        return mmr_list

    # ── Status / Introspection ────────────────────────────────────────────────

    def list_models(self) -> list[str]:
        with self._lock:
            return list(self._entries.keys())

    def model_count(self) -> int:
        with self._lock:
            return len(self._entries)

    def active_count(self) -> int:
        with self._lock:
            return sum(1 for e in self._entries.values() if e.is_ready())

    def status(self) -> dict:
        """
        Status semua model — untuk REST API /api/ml/status.

        Tiap model menyertakan labels dari config-nya → dashboard bisa tahu
        label apa yang diprediksi tanpa perlu hardcode di frontend.

        {
            "total": 3,
            "active": 2,
            "models": {
                "activity_classifier": {
                    "enabled": true,
                    "loaded": true,
                    "labels": ["Sitting", "Walking", "Lying Down", "Fall"],
                    ...
                },
                "stress_monitor": {
                    "enabled": true,
                    "loaded": true,
                    "labels": ["Low", "Medium", "High"],
                    ...
                }
            }
        }
        """
        with self._lock:
            entries = list(self._entries.values())

        models_info = {e.model_name: e.info() for e in entries}
        return {
            "total":  len(entries),
            "active": sum(1 for e in entries if e.is_ready()),
            "models": models_info,
        }

    def stats_all(self) -> dict[str, dict]:
        """Statistik inferensi semua model (dari engine.stats())."""
        with self._lock:
            entries = list(self._entries.values())
        return {
            e.model_name: e.engine.stats()
            for e in entries
            if e.engine.is_loaded
        }

    def summary_str(self) -> str:
        s = self.status()
        names = list(s["models"].keys())
        return f"[Registry] {s['active']}/{s['total']} active | {names}"

import sys
import os
from pathlib import Path

# Masukkan folder server ke python path
sys.path.insert(0, os.path.abspath("server"))

from apps.ml_inference import ModelRegistry

def test_model_registry_scan():
    # Inisialisasi registry dan scan folder models
    registry = ModelRegistry()
    model_dir = Path(__file__).resolve().parents[1] / "apps" / "ml_inference" / "models"
    registry.scan(model_dir, recursive=True)

    # Cetak status pemuatan model (akan muncul jika pytest gagal atau dijalankan dengan flag -s)
    print("\n=== STATUS MODEL REGISTRY ===")
    print(registry.summary_str())
    
    # Validasi jumlah model terdaftar
    assert registry.model_count() > 0, "Tidak ada model yang ditemukan dalam folder models!"
    assert registry.active_count() > 0, "Tidak ada model yang berhasil di-load dengan status aktif!"
    
    # Validasi status tiap model
    status = registry.status()
    for name, info in status["models"].items():
        assert info["loaded"] is True, f"Model {name} gagal di-load! Error: {info['load_error']}"
        assert info["load_error"] is None, f"Model {name} memiliki error: {info['load_error']}"
        print(f"Model '{name}' berhasil dimuat dengan {info['n_features']} fitur. Kelas: {info['labels']}")

# Graph Report - server  (2026-05-22)

## Corpus Check
- 50 files · ~20,522 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 478 nodes · 731 edges · 33 communities (22 shown, 11 thin omitted)
- Extraction: 92% EXTRACTED · 8% INFERRED · 0% AMBIGUOUS · INFERRED: 55 edges (avg confidence: 0.77)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `6fb28661`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- [[_COMMUNITY_Community 0|Community 0]]
- [[_COMMUNITY_Community 1|Community 1]]
- [[_COMMUNITY_Community 2|Community 2]]
- [[_COMMUNITY_Community 3|Community 3]]
- [[_COMMUNITY_Community 4|Community 4]]
- [[_COMMUNITY_Community 5|Community 5]]
- [[_COMMUNITY_Community 6|Community 6]]
- [[_COMMUNITY_Community 7|Community 7]]
- [[_COMMUNITY_Community 8|Community 8]]
- [[_COMMUNITY_Community 9|Community 9]]
- [[_COMMUNITY_Community 10|Community 10]]
- [[_COMMUNITY_Community 11|Community 11]]
- [[_COMMUNITY_Community 12|Community 12]]
- [[_COMMUNITY_Community 13|Community 13]]
- [[_COMMUNITY_Community 14|Community 14]]
- [[_COMMUNITY_Community 15|Community 15]]
- [[_COMMUNITY_Community 16|Community 16]]
- [[_COMMUNITY_Community 17|Community 17]]
- [[_COMMUNITY_Community 19|Community 19]]
- [[_COMMUNITY_Community 20|Community 20]]
- [[_COMMUNITY_Community 22|Community 22]]
- [[_COMMUNITY_Community 24|Community 24]]
- [[_COMMUNITY_Community 25|Community 25]]
- [[_COMMUNITY_Community 26|Community 26]]
- [[_COMMUNITY_Community 29|Community 29]]
- [[_COMMUNITY_Community 30|Community 30]]
- [[_COMMUNITY_Community 31|Community 31]]
- [[_COMMUNITY_Community 32|Community 32]]

## God Nodes (most connected - your core abstractions)
1. `StorageManager` - 27 edges
2. `_make_db()` - 23 edges
3. `_make_assessor()` - 22 edges
4. `ValidatorRegistry` - 17 edges
5. `_make_valid_imu()` - 17 edges
6. `BroadcastHub` - 13 edges
7. `_MonotonicityTracker` - 13 edges
8. `_make_full_results_and_measurements()` - 13 edges
9. `_make_results()` - 13 edges
10. `WindowReport` - 12 edges

## Surprising Connections (you probably didn't know these)
- `BroadcastHub` --uses--> `StorageManager`  [INFERRED]
  apps/dashboard/hub.py → core/storage.py
- `main()` --calls--> `setup_logging()`  [INFERRED]
  apps/reconstruct/__main__.py → core/logger.py
- `main()` --calls--> `ValidatorRegistry`  [INFERRED]
  apps/reconstruct/__main__.py → core/validator.py
- `main()` --calls--> `StorageManager`  [INFERRED]
  apps/reconstruct/__main__.py → core/storage.py
- `_make_assessor()` --calls--> `QualityAssessor`  [INFERRED]
  tests/test_quality.py → core/quality.py

## Communities (33 total, 11 thin omitted)

### Community 0 - "Community 0"
Cohesion: 0.05
Nodes (61): _layer1_schema(), _layer2_length(), _layer3_finite(), _layer5_whitelist(), _MonotonicityTracker, Simpan ts terakhir yang valid per node.     Instance ini dimiliki oleh Validato, Cek apakah ts valid dibandingkan ts terakhir untuk node ini.          Returns:, Reset state node tertentu (misalnya saat reboot terdeteksi). (+53 more)

### Community 1 - "Community 1"
Cohesion: 0.06
Nodes (31): _get(), _get_float(), _get_int(), Ambil env var dengan fallback ke default yang aman untuk lokal., Ambil env var sebagai int., Ambil env var sebagai float., Public API server core.  apps/ HANYA boleh import dari sini, bukan dari submodul, QualityFlag (+23 more)

### Community 2 - "Community 2"
Cohesion: 0.1
Nodes (40): _encode(), _make_assessor(), _make_full_results_and_measurements(), _make_phi(), _make_sparse_signal(), Jika x_hat salah (noise), residual besar → LOW_QUALITY atau CRITICAL., Rekonstruksi sangat buruk → CRITICAL.     Paksa dengan x_hat = 0 sehingga resid, sparsity_ratio = fraksi elemen non-nol. (+32 more)

### Community 3 - "Community 3"
Cohesion: 0.06
Nodes (23): QualityAssessor, Hitung metrik kualitas rekonstruksi CS.      Args:         phi : np.ndarray (, Statistik akumulatif sejak assessor dibuat.         Berguna untuk print ke cons, Satu baris ringkasan statistik global., listener.py — MQTT subscribe + dispatch ke NodeState.  Tanggung jawab tunggal:, Jalankan MQTT listener (blocking — panggil dari thread atau main).      Args:, run(), main() (+15 more)

### Community 4 - "Community 4"
Cohesion: 0.16
Nodes (27): api(), fetchDB(), fetchEvents(), fetchMetrics(), fetchNodeWindows(), fetchStatus(), fetchAll(), init() (+19 more)

### Community 5 - "Community 5"
Cohesion: 0.12
Nodes (30): _FakeMetric, _FakeReport, _make_db(), _make_results(), Nilai ndarray harus bisa dibaca kembali dari DB., Metrik dari WindowReport harus tersimpan., Tanpa report, kolom metrik harus NULL., get_last_windows harus filter signal dengan benar. (+22 more)

### Community 6 - "Community 6"
Cohesion: 0.08
Nodes (25): get_all_events(), get_node_events(), _node_or_404(), Event log untuk satu node, opsional filter per tipe., Semua event terbaru dari semua node, opsional filter per tipe., delete_node_data(), purge_old(), Hapus baris windows dan events yang lebih lama dari `max_age_hours`.     Default (+17 more)

### Community 7 - "Community 7"
Cohesion: 0.11
Nodes (14): ABC, BaseInferenceEngine, FeatureVector, InferenceEngine, PredictionResult, core/inference.py — Interface untuk ML inference engine.  Modul ini menyediakan, Placeholder ML engine.      Mengembalikan prediksi rule-based sederhana berdasar, Placeholder: akan load model ONNX/TFLite di sini.          TODO: Implementasi ak (+6 more)

### Community 8 - "Community 8"
Cohesion: 0.13
Nodes (10): Metrik kualitas untuk satu sinyal dalam satu window., Ringkasan satu baris untuk logging., Kumpulan SignalMetric untuk semua sinyal dalam satu window.     Dibuat oleh Qua, Rata-rata relative_error semua sinyal yang bisa dinilai., Satu baris ringkasan untuk console log.          Contoh output:           [Q], Satu baris per sinyal untuk log verbose., Hitung metrik kualitas untuk satu sinyal.          Args:             signal :, Hitung metrik untuk semua sinyal dalam satu window sekaligus.          Args: (+2 more)

### Community 9 - "Community 9"
Cohesion: 0.12
Nodes (18): _build_hadamard(), build_psi(), build_theta(), generate_phi(), _lcg_generator(), omp(), Matrix IDCT orthonormal Ψ (n × n).      Kenapa DCT bukan DFT kompleks?       Sin, Θ = Φ · Ψ  (m × n), murni real.      Returns:         theta : np.ndarray (m × n) (+10 more)

### Community 10 - "Community 10"
Cohesion: 0.1
Nodes (19): Test konsistensi CS encoder: router, gaussian, lasso., Φ harus berukuran (M, N)., Θ = Φ·Ψ harus berukuran (M, N)., Ψ (basis DCT) harus berukuran (N, N)., Rekonstruksi dari y(M,) harus menghasilkan x_hat(N,)., Input nol harus menghasilkan output nol., Harus menerima list Python, bukan hanya ndarray., Residual dari sinyal sparse harus kecil (< 0.5). (+11 more)

### Community 11 - "Community 11"
Cohesion: 0.13
Nodes (6): BroadcastHub, Thread-safe hub untuk push pesan ke semua WebSocket client., Simpan referensi event loop., Kirim data ke semua client, hapus yang sudah disconnect., Dipanggil dari MQTT thread., Dipanggil dari MQTT thread.

### Community 12 - "Community 12"
Cohesion: 0.14
Nodes (12): _combined_lifespan(), main(), main_app.py — Orkestrator satu proses: FastAPI + MQTT worker dalam satu perintah, Jalankan semua sistem server dalam satu proses., Blocking MQTT loop — dijalankan di background thread., Ganti lifespan dashboard_server dengan versi yang juga start MQTT thread., _run_mqtt_thread(), get_logger() (+4 more)

### Community 13 - "Community 13"
Cohesion: 0.25
Nodes (8): build_psi(), build_theta(), generate_phi(), Rekonstruksi sinyal x̂ dari measurement y menggunakan LASSO.      Args:, Bangkitkan matrix pengukuran Φ (m × n) menggunakan LCG + Box-Muller.      Identi, Matrix IDCT orthonormal Ψ (n × n)., Θ = Φ · Ψ      Returns:         theta : np.ndarray (m × n)         psi   : np.nd, reconstruct()

### Community 14 - "Community 14"
Cohesion: 0.48
Nodes (6): _ms_ago(), _now_ms(), Stream real-time setiap window selesai rekonstruksi., Stream event anomali real-time (LOW_QUALITY, CRITICAL, VALIDATION_ERROR, dll)., ws_events(), ws_stream()

### Community 15 - "Community 15"
Cohesion: 0.33
Nodes (4): Public API for Compressive Sensing package. Other modules should import CS conce, Rekonstruksi sinyal menggunakan OMP.          Args:             y : (m,) measure, Rekonstruksi sinyal menggunakan LASSO.          Args:             y : (m,) measu, reconstruct()

### Community 16 - "Community 16"
Cohesion: 0.4
Nodes (4): notify_event(), notify_window(), Dipanggil setelah window selesai direkonstruksi.     Push payload ke semua /ws/s, Dipanggil saat ada event anomali atau validasi gagal.     Push ke semua /ws/even

## Knowledge Gaps
- **11 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `StorageManager` connect `Community 1` to `Community 8`, `Community 3`, `Community 11`, `Community 5`?**
  _High betweenness centrality (0.208) - this node is a cross-community bridge._
- **Why does `main()` connect `Community 3` to `Community 0`, `Community 1`, `Community 12`?**
  _High betweenness centrality (0.158) - this node is a cross-community bridge._
- **Why does `QualityAssessor` connect `Community 3` to `Community 8`, `Community 1`, `Community 2`?**
  _High betweenness centrality (0.154) - this node is a cross-community bridge._
- **Are the 10 inferred relationships involving `StorageManager` (e.g. with `BroadcastHub` and `WindowReport`) actually correct?**
  _`StorageManager` has 10 INFERRED edges - model-reasoned connections that need verification._
- **Are the 9 inferred relationships involving `ValidatorRegistry` (e.g. with `main()` and `test_integration_full_pipeline()`) actually correct?**
  _`ValidatorRegistry` has 9 INFERRED edges - model-reasoned connections that need verification._
- **What connects `server/__main__.py  Entry point tunggal untuk seluruh sistem server.  Jalankan d`, `main_app.py — Orkestrator satu proses: FastAPI + MQTT worker dalam satu perintah`, `Blocking MQTT loop — dijalankan di background thread.` to the rest of the system?**
  _156 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Community 0` be split into smaller, more focused modules?**
  _Cohesion score 0.05 - nodes in this community are weakly interconnected._
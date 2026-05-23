# Task Breakdown — Dashboard Health Monitor

> Dikerjakan secara berurutan. Setiap task memiliki kriteria selesai (Done When) yang terukur.
> Baca `implementation_plan.md` terlebih dahulu sebelum mengerjakan task apapun.

---

## FASE 1 — Backend Extension

### [x] TASK-01: Endpoint `GET /api/ml/status`

**File:** `server/apps/dashboard/routes/ml.py` (baru)
**Register di:** `server/apps/dashboard/routes/__init__.py`

**Yang dikerjakan:**
- Buat router FastAPI baru
- Import `ModelRegistry` dari `apps.ml_inference`
- Expose status registry via GET endpoint
- Handle kasus registry belum ada model (kembalikan total=0, bukan error)

**Implementasi:**
```python
# GET /api/ml/status
# Response: status() dari ModelRegistry singleton
# Registry singleton harus dibuat di hub.py atau main_app.py
```

**Catatan:**
- Registry perlu dibuat sebagai singleton di `apps/dashboard/hub.py` (tambah `registry = ModelRegistry()`)
- `registry.scan("apps/ml_inference/models/", recursive=True)` dipanggil saat startup
- Jika folder models/ kosong, endpoint tetap return `{"total": 0, "active": 0, "models": {}}`

**Done When:**
- `curl http://localhost:8000/api/ml/status` mengembalikan JSON valid
- Tidak error meski folder models/ kosong

---

### [x] TASK-02: Endpoint `GET /api/nodes/{node_id}/activity`

**File:** `server/apps/dashboard/routes/nodes.py` (modifikasi, tambah route baru)

**Yang dikerjakan:**
- Tambah endpoint baru di router nodes yang sudah ada
- Query ke SQLite: ambil semua windows untuk node tersebut dalam N jam terakhir
- Grup berdasarkan label ML yang paling dominan per window
- Collapse window berurutan dengan label sama menjadi satu "segment aktivitas"

**Query Logic:**
```
1. Ambil semua row dari tabel `windows` WHERE node_id=? AND ts_server_ms > cutoff
   ORDER BY window_num ASC, signal='ir' (untuk dapat hr/spo2/finger)
2. Untuk setiap window_num, ambil label ML dari field yang akan ditambahkan
   (atau gunakan quality_flag sebagai proxy sementara jika ML belum ada)
3. Collapse: jika window N dan N+1 punya label sama, gabung jadi satu segment
4. Return list segment dengan start_ms, end_ms, label, duration_s, avg_confidence
```

**Catatan penting:** Tabel `windows` belum punya kolom untuk label ML. Dua opsi:
- **Opsi A (recommended):** Tambah kolom `ml_label` dan `ml_confidence` ke tabel windows di `storage.py`, isi saat `save_window()` dipanggil (jika ML result tersedia)
- **Opsi B (quick):** Endpoint ini hanya return data quality flag sebagai "aktivitas" sementara, nanti diupdate saat ML pipeline tersambung

**Done When:**
- `GET /api/nodes/1/activity?hours=24` mengembalikan JSON valid
- Format response sesuai spesifikasi di `implementation_plan.md`
- Handle node tidak ada (404)

---

### [x] TASK-03: Extend WebSocket Payload dengan `ml_results`

**File:** `server/apps/reconstruct/notifier.py` (modifikasi `notify_window`)
**File:** `server/apps/reconstruct/processor.py` (modifikasi `process_window`)

**Yang dikerjakan:**

Di `processor.py`:
```python
# Setelah quality assessment, tambah ML inference:
from apps.ml_inference import ModelRegistry
from apps.ml_inference.adapter import from_processor

# (registry sebagai singleton, import dari hub atau parameter)
window_input = from_processor(
    node_id=node_id, window_num=window_num,
    imu_data=imu_data, ppg_data=ppg_data,
    results=results,
)
ml_result = registry.predict_all(window_input)  # MultiModelResult
```

Di `notifier.py`:
```python
# Tambah field ml_results ke payload WebSocket
data["ml_results"] = ml_result.to_dict()["models"] if ml_result else {}
```

**Catatan:**
- Registry singleton harus di-share antara reconstruct dan dashboard
- Jika registry kosong (tidak ada model), `ml_results` = `{}` (bukan error)
- Jangan break existing functionality jika ML gagal — wrap dalam try/except

**Done When:**
- WebSocket message type "window" mengandung field `ml_results`
- Jika tidak ada model ML, field `ml_results` = `{}` (empty dict, bukan null)
- Existing quality/vitals data tidak terpengaruh

---

### [x] TASK-04: Singleton Registry (Shared antara Reconstruct & Dashboard)

**File:** `server/apps/dashboard/hub.py` (modifikasi)
**File:** `server/apps/main_app.py` (modifikasi startup)

**Yang dikerjakan:**
- Tambah `registry = ModelRegistry()` di `hub.py`
- Di startup lifespan `main_app.py`, panggil `registry.scan("apps/ml_inference/models/", recursive=True)`
- Import registry di `processor.py` dari `apps.dashboard.hub`

**Catatan:**
- Pola yang sama dengan `storage` dan `hub` yang sudah ada
- Thread-safe sudah di-handle oleh `ModelRegistry` internal lock

**Done When:**
- Server start tanpa error meski folder models/ kosong
- `registry` bisa diakses dari `processor.py` dan `routes/ml.py`

---

## FASE 2 — CSS & Design System

### [x] TASK-05: Rombak `style.css` — Design System Baru

**File:** `server/static/css/style.css` (rombak total)

**Yang dikerjakan:**
- Pertahankan semua CSS variables yang ada, tambahkan yang baru
- Variabel baru untuk EKG chart, ML panel, node card state
- Hapus styling komponen lama yang akan diganti
- Tambah utility classes: `.text-danger`, `.text-warn`, `.text-ok`, `.glow-danger`, dll
- Tambah CSS animations: `@keyframes glow-pulse` (untuk alert), `@keyframes slide-in-right`, `@keyframes fade-in-up`
- Responsive breakpoints untuk layar kecil

**Variabel CSS Baru yang Harus Ada:**
```css
--ekg-bg: #060809;
--ekg-grid: rgba(0, 212, 170, 0.05);
--ekg-line: #00d4aa;
--ekg-line-ir: #ff6bc6;

--card-active-border: rgba(0, 212, 170, 0.4);
--card-alert-border: rgba(255, 77, 77, 0.6);
--card-alert-glow: 0 0 20px rgba(255, 77, 77, 0.3);

--timeline-row-height: 32px;
--timeline-label-width: 80px;
```

**Done When:**
- File CSS valid, tidak ada syntax error
- Semua variabel yang direferensikan di komponen lain sudah terdefinisi

---

### [x] TASK-06: Buat `components.css` — Komponen Reusable

**File:** `server/static/css/components.css` (baru)

**Yang dikerjakan:**
- Styling untuk Node Card (semua state: default, active, online, offline, alert)
- Styling untuk Badge (ML label badge, event type badge)
- Styling untuk Chart Container (wrapper ECharts)
- Styling untuk Alert Banner (notifikasi pojok kanan atas)
- Styling untuk Tooltip custom
- Styling untuk Tab / Toggle (untuk pilih sinyal di EKG)

**Done When:**
- Komponen bisa dipakai hanya dengan class HTML, tanpa inline style
- Card bisa dirender manual di browser dan terlihat benar

---

### [x] TASK-07: Buat `charts.css` — ECharts Container Styling

**File:** `server/static/css/charts.css` (baru)

**Yang dikerjakan:**
- Container sizing untuk setiap chart (EKG strip: height 200px, vitals: 180px, dll)
- Loading state styling (skeleton/shimmer saat data belum ada)
- No-data state styling
- Responsive height adjustment

**Done When:**
- Container chart tidak collapse saat ECharts belum di-init

---

## FASE 3 — HTML Layout

### [x] TASK-08: Rombak `index.html` — Layout Baru

**File:** `server/static/index.html` (rombak total)

**Yang dikerjakan:**
- Struktur HTML sesuai layout di `implementation_plan.md`
- Load ECharts dari CDN: `https://cdnjs.cloudflare.com/ajax/libs/echarts/5.4.3/echarts.min.js`
- Pertahankan Chart.js untuk kompatibilitas mundur (hapus jika semua chart sudah ECharts)
- Semua `id` attribute yang direferensikan JS harus ada
- Struktur DOM:
  ```html
  <header id="commandBar">...</header>
  <div class="layout">
    <aside id="nodeList">...</aside>
    <main id="detailPanel">
      <section id="ekgSection">...</section>
      <section id="midSection">
        <div id="mlPanel">...</div>
        <div id="vitalsPanel">...</div>
      </section>
      <section id="timelineSection">...</section>
      <section id="bottomSection">
        <div id="qualityGrid">...</div>
        <div id="eventLog">...</div>
      </section>
    </main>
  </div>
  <div id="alertBanner"></div>
  <div id="toastWrap"></div>
  ```

**Done When:**
- HTML valid, tidak ada tag yang tidak ditutup
- Layout terlihat benar di browser meski tanpa data (placeholder states)
- Semua CSS ter-load tanpa 404

---

## FASE 4 — JavaScript Core

### [x] TASK-09: Update `state.js` — State Management Baru

**File:** `server/static/js/state.js` (modifikasi)

**Yang dikerjakan:**
- Tambah state untuk ML: `mlModels`, `mlLabels`, `mlLabelColors`
- Tambah state untuk node: `nodes` (map node_id → data), `selectedNodes` (support multi-select)
- Tambah state untuk alert: `alerts`, `alertCount`
- Tambah state untuk EKG buffer: `ekgBuffer` (map node_id → circular buffer data)
- Helper function `getLabelColor(label)` — return warna CSS berdasarkan nama label

**Aturan `getLabelColor`:**
```javascript
// Priority check berdasarkan keyword
const DANGER_KEYWORDS = ['jatuh', 'fall', 'critical', 'tachycardia', 'low_spo2'];
const WARN_KEYWORDS   = ['sedang', 'medium', 'bradycardia', 'low'];
// Jika tidak match → generate dari hash nama label
```

**Done When:**
- `state.js` bisa di-import oleh semua modul lain tanpa circular dependency
- `getLabelColor('jatuh')` return `var(--danger)`

---

### [x] TASK-10: Update `api.js` — Tambah ML Endpoints

**File:** `server/static/js/api.js` (modifikasi)

**Yang dikerjakan:**
- Tambah `fetchMLStatus()` — panggil `/api/ml/status`, simpan ke `state.mlModels`
- Tambah `fetchNodeActivity(nodeId, hours)` — panggil `/api/nodes/{id}/activity`
- Update `fetchStatus()` agar populate `state.nodes` map (bukan hanya render UI)
- Tambah error handling yang lebih robust (retry logic untuk WS reconnect)

**Done When:**
- `fetchMLStatus()` mengisi `state.mlModels` dengan benar
- Semua fetch function handle 404 / network error dengan graceful fallback

---

### [x] TASK-11: Update `ws.js` — Handle Payload Baru

**File:** `server/static/js/ws.js` (modifikasi)

**Yang dikerjakan:**
- Handle field `ml_results` di message type "window"
- Update EKG buffer saat window baru masuk
- Trigger alert system jika ada label CRITICAL dari ML
- Exponential backoff saat reconnect (1s, 2s, 4s, 8s, max 30s)

**Done When:**
- `ml_results` di-parse dan diteruskan ke `ml.js`
- EKG buffer terupdate setiap window masuk via WS
- Reconnect berfungsi dengan backoff

---

## FASE 5 — Komponen UI

### [x] TASK-12: Buat `nodes.js` — Node Card Management

**File:** `server/static/js/nodes.js` (baru)

**Yang dikerjakan:**
- `renderNodeList(nodes)` — generate DOM untuk semua node cards dari data API
- `updateNodeCard(nodeId, windowData)` — update satu card saat window baru masuk (HR, SpO2, label ML, sparkline)
- `selectNode(nodeId)` — set node aktif, update detail panel
- `setNodeAlert(nodeId, isAlert)` — toggle CSS alert state pada card
- Mini sparkline: ECharts instance kecil (tanpa axis, tanpa tooltip) per card, update rolling 30 titik

**Card DOM Structure:**
```html
<div class="node-card" data-node-id="1">
  <div class="card-header">
    <span class="node-indicator online"></span>
    <span class="node-name">Node 1</span>
    <span class="node-ago">2s ago</span>
  </div>
  <div class="card-activity">
    <span class="activity-label" style="color: var(--ok)">JALAN</span>
    <span class="activity-confidence">96.2%</span>
  </div>
  <div class="card-vitals">
    <span class="vital-hr">78 bpm</span>
    <span class="vital-spo2">98.1%</span>
  </div>
  <div class="card-sparkline" id="sparkline-1"></div>
  <div class="card-footer">Win #1042</div>
</div>
```

**Done When:**
- Node cards ter-render dari data `/api/status`
- Card update real-time saat window baru via WS
- Select node berfungsi, card active state benar

---

### [x] TASK-13: Buat `charts/ekg_strip.js` — EKG Scrolling Chart

**File:** `server/static/js/charts/ekg_strip.js` (baru)

**Yang dikerjakan:**
- Init ECharts di container `#ekgSection`
- Buffer circular: max 300 titik per sinyal
- Render dua series: SMV (sinyal utama) + IR (overlay, Y-axis kanan)
- Update chart via `chart.setOption()` dengan `notMerge: false` untuk performa
- Signal selector dropdown: pilih sinyal yang ditampilkan (SMV default, atau pilih per axis)
- Marker vertikal (markLine) saat ada event quality LOW_QUALITY/CRITICAL

**ECharts Config Skeleton:**
```javascript
{
  backgroundColor: 'var(--ekg-bg)',
  grid: { top: 10, bottom: 30, left: 50, right: 60 },
  xAxis: { type: 'category', data: timestamps, boundaryGap: false },
  yAxis: [
    { type: 'value', name: 'SMV', position: 'left' },
    { type: 'value', name: 'IR', position: 'right' }
  ],
  series: [
    { name: 'SMV', type: 'line', smooth: true, symbol: 'none', areaStyle: {...} },
    { name: 'IR',  type: 'line', smooth: true, symbol: 'none', yAxisIndex: 1 }
  ]
}
```

**Done When:**
- Chart ter-render saat init (dengan data dummy jika belum ada live data)
- Update smooth saat window baru masuk
- Tidak ada memory leak (buffer size tetap max 300)

---

### [x] TASK-14: Buat `charts/probability_bar.js` — ML Confidence Chart

**File:** `server/static/js/charts/probability_bar.js` (baru)

**Yang dikerjakan:**
- Terima `mlModels` dari state untuk tahu struktur (nama model + labels)
- Render satu ECharts horizontal bar chart per model aktif
- Container: `#mlPanel`, buat sub-div per model secara dinamis
- Update saat `ml_results` baru masuk via WS
- Handle state `skipped: true` — tampilkan badge "SKIP" dengan reason
- Handle model baru yang muncul di `ml_results` (model yang baru di-register)

**Visual Detail:**
- Bar warnanya berubah berdasarkan label (pakai `getLabelColor` dari `state.js`)
- Label dengan confidence tertinggi di-highlight (bold + slightly larger bar)
- Animasi easeOut 300ms saat update

**Done When:**
- Panel render dengan data dummy (tidak perlu koneksi WS)
- Update animasi bekerja saat dipanggil `updateMLPanel(mlResults)`
- Handle empty/skip state dengan graceful UI

---

### [x] TASK-15: Buat `charts/vitals_trend.js` — HR + SpO2 Chart

**File:** `server/static/js/charts/vitals_trend.js` (baru)

**Yang dikerjakan:**
- ECharts line chart dengan dual Y-axis
- Series 1: HR (bpm) — warna merah/oranye, Y-axis kiri (range 0–200)
- Series 2: SpO2 (%) — warna cyan, Y-axis kanan (range 85–100)
- Reference lines (markLine): HR 60, HR 100, SpO2 95
- Area shading merah di zona bahaya (HR < 60, HR > 100, SpO2 < 95)
- Buffer: 60 titik terakhir

**Done When:**
- Chart render dengan data dummy
- Area danger zone terlihat jelas
- Update saat window baru masuk

---

### [x] TASK-16: Buat `charts/activity_timeline.js` — Gantt Chart

**File:** `server/static/js/charts/activity_timeline.js` (baru)

**Yang dikerjakan:**
- ECharts custom series atau bar chart horizontal untuk gantt
- Data dari `fetchNodeActivity(nodeId, hours)`
- Sumbu X: waktu (format jam:menit)
- Satu baris per node yang dipilih
- Warna balok dari `getLabelColor(label)`
- Tooltip saat hover: label, durasi, avg confidence
- Toggle: 1 jam / 6 jam / 24 jam (ubah range sumbu X)
- Refresh setiap 60 detik via `setInterval`

**Done When:**
- Timeline render dari data API
- Toggle range berfungsi
- Tooltip informatif saat hover balok

---

### [x] TASK-17: Buat `ml.js` — ML Panel Orchestrator

**File:** `server/static/js/ml.js` (baru)

**Yang dikerjakan:**
- `initMLPanel()` — panggil `fetchMLStatus()`, init chart per model
- `updateMLPanel(mlResults)` — diterima dari `ws.js`, diteruskan ke `probability_bar.js`
- `handleNewModel(modelName, modelInfo)` — jika model baru muncul di runtime, tambah section baru
- Header section per model: nama model + versi + badge jumlah label

**Done When:**
- Panel init dengan data dari `/api/ml/status`
- Update real-time dari WebSocket bekerja
- Model baru yang muncul di runtime ter-render tanpa refresh halaman

---

### [x] TASK-18: Buat `alerts.js` — Alert System

**File:** `server/static/js/alerts.js` (baru)

**Yang dikerjakan:**
- `triggerAlert(nodeId, type, message)` — dipanggil oleh WS handler atau ML panel
- Tambah alert banner di `#alertBanner` (stack, max 5 terlihat)
- Auto-dismiss setelah 8 detik
- Set node card ke alert state via `nodes.js`
- Update alert counter di command bar
- Web Audio API tone: frekuensi 880Hz, durasi 200ms, gain rendah
- `clearAlert(nodeId)` — dipanggil saat kondisi kembali normal

**Alert Trigger Conditions:**
- ML label confidence > 0.7 untuk label yang mengandung keyword danger
- Event type CRITICAL dari `/ws/events`
- quality report `has_critical()` = true dari window WS

**Done When:**
- Alert muncul dengan animasi saat trigger dipanggil
- Suara berbunyi (Web Audio API, bukan file audio)
- Auto-dismiss berfungsi
- Alert counter di command bar terupdate

---

## FASE 6 — Update `ui.js` dan `app.js`

### [x] TASK-19: Rombak `ui.js` — Koordinator UI

**File:** `server/static/js/ui.js` (rombak)

**Yang dikerjakan:**
- Hapus fungsi yang sudah dipindah ke modul spesifik (`nodes.js`, `ml.js`, dll)
- Pertahankan: `toast()`, `flash()`, `setWSStatus()`, `appendEvent()`
- Tambah: `updateCommandBar(serverStats)` — update uptime, window count, alert badge
- Tambah: `renderQualityGrid(signals)` — refactor dari yang sudah ada, styling baru
- Semua DOM manipulation via helper functions, tidak ada `document.getElementById` yang tersebar

**Done When:**
- Tidak ada fungsi duplikat dengan modul lain
- Semua fungsi terdokumentasi dengan JSDoc singkat

---

### [x] TASK-20: Update `app.js` — Orchestrator Utama

**File:** `server/static/js/app.js` (modifikasi)

**Yang dikerjakan:**
- Import semua modul baru
- Update `init()`:
  1. Fetch server status
  2. Fetch ML status (init ML panel)
  3. Connect WebSocket
  4. Start polling interval (15s)
- Route WS messages ke handler yang tepat:
  - `type: "window"` → `nodes.js`, `ekg_strip.js`, `vitals_trend.js`, `ml.js`, `alerts.js`
  - `type: "snapshot"` → `nodes.js`
  - `type: "event"` → `ui.js` appendEvent + `alerts.js`

**Done When:**
- `init()` berjalan tanpa error di console
- Semua modul ter-import dengan benar
- Data flow dari WS ke semua komponen berfungsi

---

## FASE 7 — Polish & Quality

### TASK-21: Responsiveness & Edge Cases

**Yang dikerjakan:**
- Layout responsive untuk layar 1280px, 1440px, 1920px
- Handle kondisi: node offline (last seen > 30s), sinyal hilang sebagian, finger tidak terdeteksi
- Loading skeleton saat data pertama kali dimuat
- Error state jika API tidak bisa dijangkau
- Empty state yang informatif (bukan layar kosong)

**Done When:**
- Tidak ada layout broken di resolusi 1280px
- Semua edge case ditampilkan dengan pesan yang jelas

---

### TASK-22: Performance Optimization

**Yang dikerjakan:**
- Pastikan tidak ada memory leak di EKG chart (buffer size terbatas)
- Debounce update chart maksimal 60fps (requestAnimationFrame)
- Lazy init ECharts (init saat section pertama kali visible, bukan saat load)
- Dispose ECharts instance saat node di-unselect

**Done When:**
- Memory usage browser tidak naik terus setelah 10 menit streaming
- CPU usage < 15% di mesin pengembang saat streaming 2 node

---

### TASK-23: Testing Manual Checklist

Sebelum dinyatakan selesai, verifikasi manual:

**Koneksi:**
- [ ] Dashboard load tanpa error di console
- [ ] WebSocket terkoneksi dan badge "connected" muncul
- [ ] Reconnect otomatis setelah koneksi putus

**Node Management:**
- [ ] Node card muncul saat node pertama kirim data
- [ ] Node card kedua muncul tanpa refresh
- [ ] Card update HR/SpO2/label setiap window
- [ ] Klik card ganti tampilan di detail panel

**Charts:**
- [ ] EKG strip scroll smooth, tidak ada glitch
- [ ] ML panel update saat window baru masuk
- [ ] Vitals trend menampilkan garis referensi yang benar
- [ ] Activity timeline load dari API

**Alert:**
- [ ] Alert muncul saat event CRITICAL masuk
- [ ] Node card berubah merah saat alert
- [ ] Alert counter increment
- [ ] Alert auto-dismiss setelah 8 detik

**ML Dynamic:**
- [ ] Tambah model baru ke folder models/ → refresh → panel baru muncul
- [ ] Label baru di config JSON → warna ter-generate otomatis

---

## Dependency Graph

```
TASK-04 (registry singleton)
  └── TASK-01 (ml/status endpoint)
  └── TASK-03 (ws payload extension)
        └── TASK-11 (ws.js update)

TASK-02 (activity endpoint) ← independent

TASK-05 + TASK-06 + TASK-07 (CSS) ← independent, bisa paralel
  └── TASK-08 (HTML layout)
        └── semua task JS

TASK-09 (state.js)
  └── TASK-10 (api.js)
  └── TASK-12 (nodes.js)
  └── TASK-13 (ekg_strip.js)
  └── TASK-14 (probability_bar.js)
  └── TASK-15 (vitals_trend.js)
  └── TASK-16 (activity_timeline.js)
  └── TASK-17 (ml.js)
  └── TASK-18 (alerts.js)

TASK-12 → TASK-19 → TASK-20 (final orchestration)
TASK-21, TASK-22, TASK-23 ← setelah semua task selesai
```

---

## Catatan untuk Agent

1. **Jangan ubah file di luar `server/static/` dan `server/apps/dashboard/`** kecuali `server/apps/reconstruct/notifier.py` dan `processor.py` untuk TASK-03.

2. **Konsistensi naming:** Semua file JS baru menggunakan camelCase untuk fungsi, snake_case untuk variabel yang merepresentasikan data dari API.

3. **Tidak ada hardcode label ML.** Semua label (nama aktivitas, threshold danger) harus dibaca dari state yang diisi oleh `/api/ml/status`.

4. **ECharts CDN:** Gunakan versi yang konsisten: `https://cdnjs.cloudflare.com/ajax/libs/echarts/5.4.3/echarts.min.js`

5. **Backward compatibility:** `GET /api/status`, `/ws/stream`, dan `/ws/events` harus tetap berfungsi dengan payload yang ada — hanya extend, tidak break.

6. **Urutan pengerjaan WAJIB:** Fase 1 (backend) sebelum Fase 4+ (JS yang butuh endpoint baru). Fase 2-3 (CSS/HTML) bisa paralel dengan Fase 1.

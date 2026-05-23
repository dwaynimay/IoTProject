# Dashboard Health Monitor — Implementation Plan

## Konteks Proyek

Dashboard real-time untuk memantau sinyal sensor ESP32 (IMU + PPG) yang direkonstruksi via Compressive Sensing. Backend sudah siap: FastAPI REST + WebSocket, SQLite storage, ML inference engine (registry multi-model), dan MQTT reconstruction pipeline.

Dashboard yang akan dibangun: **clinical monitoring style** — dark theme, dense informasi, responsif terhadap data real-time. Dirancang agar sepenuhnya dinamis: node baru otomatis muncul, label ML dari config JSON langsung ter-render tanpa ubah kode frontend.

---

## Arsitektur Dashboard

### Stack Frontend

- **Vanilla JS (ES Modules)** — konsisten dengan kode yang sudah ada di `server/static/js/`
- **ECharts 5.x** — library grafik utama (CDN). Dipilih karena:
  - Native streaming data support
  - Canvas renderer (lebih performa dari SVG untuk real-time)
  - API dinamis untuk update sumbu/label tanpa re-render penuh
- **IBM Plex Mono + IBM Plex Sans** — sudah ada di CSS, pertahankan
- **CSS Custom Properties** — sistem variabel yang sudah ada, diperluas

### Struktur File Baru / Dimodifikasi

```
server/static/
├── index.html                  ← ROMBAK TOTAL (layout baru)
├── css/
│   ├── style.css               ← ROMBAK (design system baru)
│   ├── components.css          ← BARU (komponen reusable)
│   └── charts.css              ← BARU (styling ECharts container)
└── js/
    ├── app.js                  ← MODIFIKASI (init flow baru)
    ├── state.js                ← MODIFIKASI (tambah ML state)
    ├── api.js                  ← MODIFIKASI (tambah ML endpoints)
    ├── ws.js                   ← MODIFIKASI (handle ml_results payload)
    ├── ui.js                   ← ROMBAK TOTAL (komponen baru)
    ├── chart.js                ← ROMBAK TOTAL (ECharts, multi-chart)
    ├── charts/
    │   ├── ekg_strip.js        ← BARU (scrolling EKG chart)
    │   ├── probability_bar.js  ← BARU (ML confidence bars, dinamis)
    │   ├── activity_timeline.js← BARU (gantt chart historis)
    │   └── vitals_trend.js     ← BARU (HR + SpO2 line chart)
    ├── nodes.js                ← BARU (node card management)
    ├── ml.js                   ← BARU (ML panel rendering)
    └── alerts.js               ← BARU (alert system)
```

### Endpoint Backend yang Diperlukan

#### Sudah Ada (Gunakan Langsung)
- `GET /api/status` — list nodes + stats
- `GET /api/nodes/{node_id}` — detail node
- `GET /api/nodes/{node_id}/windows` — data window per sinyal
- `GET /api/nodes/{node_id}/events` — event log
- `GET /api/metrics` — server metrics
- `WS /ws/stream` — push real-time per window
- `WS /ws/events` — push real-time events

#### Perlu Ditambah di Backend
- `GET /api/ml/status` — list model aktif + labels dari registry
- `GET /api/nodes/{node_id}/activity` — riwayat aktivitas untuk timeline
- WebSocket payload `/ws/stream` diperluas dengan field `ml_results`

---

## Layout Dashboard

### Tata Letak Utama (Grid)

```
┌─────────────────────────────────────────────────────────────────┐
│  COMMAND BAR (52px) — Brand, Server Status, Alert Counter, WS  │
├──────────────┬──────────────────────────────────────────────────┤
│              │  DETAIL PANEL (area utama, 70% lebar)            │
│  NODE LIST   │  ┌─────────────────────────────────────────────┐ │
│  (220px)     │  │ EKG STRIP (scrolling sinyal, full width)    │ │
│              │  │                                              │ │
│  ┌────────┐  │  ├──────────────────┬──────────────────────────┤ │
│  │ NODE 1 │  │  │ ML INFERENCE     │ VITALS TREND             │ │
│  │ (card) │  │  │ (probability bar)│ (HR + SpO2 line chart)   │ │
│  ├────────┤  │  ├──────────────────┴──────────────────────────┤ │
│  │ NODE 2 │  │  │ ACTIVITY TIMELINE (gantt horizontal)        │ │
│  │ (card) │  │  ├─────────────────────────────────────────────┤ │
│  ├────────┤  │  │ QUALITY GRID + EVENT LOG (2 kolom)          │ │
│  │ + baru │  │  └─────────────────────────────────────────────┘ │
│  │ (auto) │  │                                                   │
│  └────────┘  │                                                   │
└──────────────┴───────────────────────────────────────────────────┘
```

---

## Spesifikasi Komponen

### 1. Command Bar

**Konten:**
- Brand dot (animasi pulse) + nama "HEALTH MONITOR"
- Badge: `N nodes active` (update real-time)
- Badge: `N alerts` (merah jika ada CRITICAL)
- Status server: uptime, avg rekonstruksi ms
- Indikator WebSocket (connected / disconnected / error)
- Input API base URL + tombol connect

**Behavior:**
- Sticky, selalu di atas
- Alert badge bergetar (CSS animation) jika ada event CRITICAL baru

### 2. Node Cards (Sidebar Kiri)

Setiap card di-generate dinamis dari `/api/status`. Node baru otomatis muncul.

**Isi satu card:**
```
┌─────────────────────────────┐
│ ● NODE 1          [ONLINE]  │
│ ─────────────────────────── │
│  JALAN            96.2%     │  ← label ML terbesar + confidence
│  ─────────────────────────  │
│  HR: 78 bpm   SpO2: 98.1%  │
│  ─────────────────────────  │
│  [sparkline 30 detik]       │
│  ─────────────────────────  │
│  Win #1042  •  0.3s ago     │
└─────────────────────────────┘
```

**Behavior:**
- Klik card → panel kanan update ke node tersebut
- Card border merah + glow jika ada alert CRITICAL
- Sparkline pakai ECharts mini (line chart inline, tanpa axis)
- Label aktivitas dan warnanya dibaca dari ML config, bukan hardcode
- Card baru muncul dengan animasi slide-in

### 3. EKG Strip (Detail Panel — Atas)

Scrolling real-time line chart, mirip monitor jantung di rumah sakit.

**Spesifikasi teknis:**
- Pakai `ECharts` dengan `dataset` yang di-shift setiap window masuk
- Tampilkan SMV (Signal Magnitude Vector = √(ax²+ay²+az²)) sebagai sinyal utama
- Overlay line IR (PPG) di axis kedua (Y kanan), warna berbeda
- Buffer: 300 titik data (≈ 5 menit di 1 sample/detik)
- Update rate: setiap window masuk via WebSocket
- Pilihan sinyal bisa diganti via dropdown (ax, ay, az, gx, gy, gz, ir, SMV)
- Garis vertikal merah saat event CRITICAL/LOW_QUALITY

**Visual:**
- Background gelap dengan grid horizontal tipis
- Warna sinyal: accent cyan untuk SMV, pink untuk IR
- Label sumbu X: waktu relatif ("30s ago", "60s ago")
- Animasi smooth saat data masuk

### 4. ML Inference Panel (Detail Panel — Tengah Kiri)

Horizontal bar chart yang sepenuhnya dinamis dari registry.

**Spesifikasi teknis:**
- Panggil `GET /api/ml/status` saat init untuk dapat daftar model + labels
- Untuk setiap model yang aktif, render satu section bar chart
- Update real-time dari field `ml_results` di WebSocket payload
- Bar panjangnya = probabilitas (0–1), tampilkan persentase di ujung

**Contoh render untuk 2 model:**
```
── ACTIVITY CLASSIFIER v1.0 ──
  JALAN     ████████████░░░░  87.3%
  DUDUK     ██░░░░░░░░░░░░░░   8.1%
  JATUH     █░░░░░░░░░░░░░░░   3.2%
  TIDUR     ░░░░░░░░░░░░░░░░   1.4%

── STRESS MONITOR v1.0 ──
  RENDAH    ████████░░░░░░░░   62%
  SEDANG    ████░░░░░░░░░░░░   31%
  TINGGI    █░░░░░░░░░░░░░░░    7%
```

**Visual:**
- Warna bar: hijau → kuning → merah berdasarkan label (threshold dari config)
- Bar animasi smooth saat update
- Label JATUH / kondisi kritis otomatis berwarna merah
- Jika model di-skip (finger not detected, dll), tampilkan badge "SKIP: finger not detected"

### 5. Vitals Trend Chart (Detail Panel — Tengah Kanan)

Line chart HR dan SpO2 selama sesi berlangsung.

**Spesifikasi:**
- Dual Y-axis: kiri untuk HR (bpm), kanan untuk SpO2 (%)
- Garis horizontal referensi: HR 60 (bradycardia bawah), HR 100 (tachycardia atas), SpO2 95% (batas normal)
- Area shading merah di zona bahaya
- Buffer: 60 window terakhir
- Update dari WebSocket

### 6. Activity Timeline (Detail Panel — Tengah Bawah)

Gantt chart horizontal menampilkan riwayat aktivitas hari ini.

**Spesifikasi:**
- Sumbu X: waktu (jam)
- Satu baris per node yang dipilih (atau semua node jika mode overview)
- Balok berwarna = aktivitas (warna dari ML label config)
- Klik balok → popup detail (durasi, confidence rata-rata)
- Data dari `GET /api/nodes/{id}/activity` (endpoint baru)
- Zoom: bisa pilih range 1 jam, 6 jam, 24 jam

### 7. Reconstruction Quality Grid

Komponen yang sudah ada, di-redesign:
- Layout 7 kolom (ax, ay, az, gx, gy, gz, ir)
- Bar vertikal (bukan horizontal) untuk visual lebih compact
- Warna: hijau (<0.10), kuning (0.10–0.25), merah (>0.25)
- Tooltip saat hover: rel_error, sparsity, snr_db

### 8. Event Log

Komponen yang sudah ada, dengan tambahan:
- Filter by type (CRITICAL, LOW_QUALITY, VALIDATION_ERROR, NODE_REGISTERED)
- Badge count per tipe
- Auto-scroll dengan pause saat user hover

### 9. Alert System

**Trigger:** Label CRITICAL dari ML, atau event CRITICAL dari quality assessor.

**Mekanisme:**
- Card node berubah warna border merah + CSS glow animation
- Banner notifikasi muncul di pojok kanan atas (stack, bisa dismiss)
- Suara notifikasi via Web Audio API (tone pendek, tidak mengganggu)
- Alert count di command bar increment
- Log alert tersimpan di state untuk review

---

## WebSocket Payload Extension

### Payload `/ws/stream` Saat Ini
```json
{
  "type": "window",
  "node_id": 1,
  "window_num": 42,
  "hr": 78,
  "spo2": 98.1,
  "finger": true,
  "elapsed_ms": 12.3,
  "quality": { "avg_rel_error": 0.08, "signals": {...} }
}
```

### Payload Setelah Diperluas (Tambahkan `ml_results`)
```json
{
  "type": "window",
  "node_id": 1,
  "window_num": 42,
  "hr": 78,
  "spo2": 98.1,
  "finger": true,
  "elapsed_ms": 12.3,
  "quality": { "avg_rel_error": 0.08, "signals": {...} },
  "ml_results": {
    "activity_classifier": {
      "label": "jalan",
      "confidence": 0.873,
      "proba": {"duduk": 0.081, "jalan": 0.873, "jatuh": 0.032, "tidur": 0.014},
      "skipped": false,
      "elapsed_ms": 1.2
    }
  }
}
```

---

## Endpoint Backend Baru

### `GET /api/ml/status`

```json
{
  "total": 2,
  "active": 2,
  "models": {
    "activity_classifier": {
      "model_name": "activity_classifier",
      "version": "1.0.0",
      "enabled": true,
      "loaded": true,
      "labels": ["duduk", "jalan", "jatuh", "tidur"],
      "n_features": 44
    }
  }
}
```

### `GET /api/nodes/{node_id}/activity`

Query params: `?hours=24` (default 24)

```json
{
  "node_id": 1,
  "hours": 24,
  "activities": [
    {
      "label": "duduk",
      "start_ms": 1717000000000,
      "end_ms": 1717001800000,
      "duration_s": 1800,
      "avg_confidence": 0.91
    }
  ]
}
```

---

## Design System

### Warna

```css
:root {
  /* Base (sudah ada, pertahankan) */
  --bg: #0a0c0f;
  --surface: #111318;
  --surface2: #181b22;
  --border: rgba(255,255,255,0.07);

  /* Sinyal */
  --sig-smv:  #00d4aa;   /* cyan — sinyal utama EKG */
  --sig-ir:   #ff6bc6;   /* pink — PPG */
  --sig-ax:   #0099ff;
  --sig-ay:   #7c6aff;
  --sig-az:   #f5a623;

  /* Status */
  --ok:       #00d4aa;
  --warn:     #f5a623;
  --danger:   #ff4d4d;
  --info:     #0099ff;

  /* ML Labels — default, override dari config */
  --label-safe:     #00d4aa;
  --label-caution:  #f5a623;
  --label-critical: #ff4d4d;
}
```

### Label Warna ML (Dinamis)

Sistem pewarnaan label ML tidak boleh hardcode. Gunakan algoritma:
1. Label yang mengandung kata kunci bahaya ("jatuh", "fall", "critical", "tachycardia") → `--danger`
2. Label yang mengandung kata kunci perhatian ("low", "bradycardia", "sedang") → `--warn`
3. Sisanya → palet warna yang di-generate dari hash nama label

---

## Urutan Implementasi yang Direkomendasikan

1. Backend endpoints baru (`/api/ml/status`, `/api/nodes/{id}/activity`)
2. Extend WebSocket payload dengan `ml_results`
3. Design system CSS baru
4. Layout HTML baru (shell tanpa data)
5. Node cards (dinamis dari `/api/status`)
6. EKG strip chart (ECharts, data dummy dulu)
7. ML inference panel (dari `/api/ml/status`)
8. Vitals trend chart
9. Activity timeline
10. Alert system
11. WebSocket integration (sambungkan semua ke live data)
12. Polish: animasi, responsive, edge cases

# PPG Models

Folder ini untuk model berbasis sinyal PPG (photoplethysmography).

## Sinyal Input

- `ir` — sinyal infrared rekonstruksi CS (64 sample)
- `hr` — heart rate (metadata dari firmware)
- `spo2` — saturasi oksigen (metadata)
- `finger` — status deteksi jari

## Rencana Model

| Model | Task | Status |
|-------|------|--------|
| `hr_classifier` | Klasifikasi kondisi HR (normal/tachycardia/bradycardia) | Belum dilatih |
| `spo2_classifier` | Klasifikasi SpO2 (normal/low_spo2) | Belum dilatih |

## Catatan Fitur PPG

Fitur yang umum untuk PPG:
- `ir_mean`, `ir_std`, `ir_rms` — statistik dasar
- `ir_peak_freq` — frekuensi dominan (FFT) → estimasi HR dari sinyal
- `ir_spectral_energy` — energi spektral

Tambahkan config dengan `"skip_if": {"finger_required": true}` agar model skip window saat jari tidak terdeteksi.

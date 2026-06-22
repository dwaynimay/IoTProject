// File: firmware/lib/HealthSensors/HeartRateMonitor.h

#pragma once
// =============================================================================
// HeartRateMonitor.h — Pipeline Deteksi Detak Jantung (Wrist PPG)
// =============================================================================
//
// Merangkai komponen DSP dari PpgDsp.h menjadi satu pipeline heart rate:
//
//   IR mentah
//     -> BandPass      (isolasi pita detak, buang DC/drift)
//     -> MotionGate    (deteksi & blokir gerakan tangan)
//     -> PeakEnvelope  (threshold adaptif, dibekukan saat motion)
//     -> BeatDetector  (rising-edge + refractory + hysteresis)
//     -> BpmEstimator  (median + smoothing + anti-deadlock)
//
// Kelas ini TIDAK menyentuh hardware — ia hanya menerima sampel IR dan
// timestamp. Dengan begitu ia bisa diuji sepenuhnya tanpa sensor fisik.
//
// CARA PAKAI:
//   HeartRateMonitor hr;
//   hr.reset();
//   // tiap sampel:
//   hr.update(irValue, millis());
//   int bpm = hr.getBpm();       // 0 jika belum siap
//   bool ok = hr.isValid();
// =============================================================================

#include "PpgDsp.h"

class HeartRateMonitor
{
public:
    HeartRateMonitor() = default;

    void reset()
    {
        _bandpass.reset();
        _envelope.reset();
        _detector.reset();
        _motion.reset();
        _imuGate.reset();
        _bpmEst.reset();
        _wasMotion  = false;
        _lastBpf    = 0.0f;
        _lastEnv    = 0.0f;
        _inMotion   = false;
        _imuMotion  = false;
        _contactMs  = 0;
        _settled    = false;
        _lastBeatMs = 0;
        _stale      = false;
        _motionStartMs = 0;
    }

    // Dipanggil saat kontak kulit pertama terdeteksi: seed filter ke nilai IR
    // awal agar tidak ada transient palsu.
    void onContact(float irValue, uint32_t nowMs)
    {
        _bandpass.seed(irValue);
        _envelope.reset();
        _detector.reset();
        _detector.syncTime(nowMs);
        _motion.reset();
        _bpmEst.softReset();
        _wasMotion = false;
        _contactMs = nowMs;
        _settled   = false;
        _lastBeatMs = 0;
        _stale      = false;
        _motionStartMs = nowMs;
    }

    // ── Input IMU (opsional) ────────────────────────────────────────────────
    // Panggil SEBELUM update() tiap siklus jika punya accelerometer.
    // accelMag = sqrt(ax^2 + ay^2 + az^2) dalam satuan g (saat diam ~1.0).
    // Jika tidak pernah dipanggil, sistem jatuh ke motion gate berbasis-PPG saja.
    void setAccel(float accelMag, uint32_t nowMs)
    {
        _imuMotion = _imuGate.process(accelMag, nowMs);
        _hasImu = true;
    }

    // Proses satu sampel IR. Mengembalikan true jika detak terdeteksi
    // pada sampel ini (interval > 0).
    bool update(float irValue, uint32_t nowMs)
    {
        // 1. Band-pass
        const float bpf = _bandpass.process(irValue);
        _lastBpf = bpf;

        // 2. Envelope detak — diupdate dulu agar MotionGate punya acuan
        //    amplitudo detak yang mapan. Saat motion terdeteksi (langkah 3),
        //    envelope berikutnya akan dibekukan agar lonjakan tak merusaknya.
        const float env = _envelope.process(bpf, _inMotion /*freeze jika motion*/);
        _lastEnv = env;

        // 3. Motion gate — gabungan IMU (langsung) + PPG (cadangan).
        //    Jika IMU tersedia, ia jadi sumber utama (lebih andal). PPG motion
        //    tetap dipakai sebagai pelengkap (OR): salah satu mendeteksi → motion.
        const bool ppgMotion = _motion.process(fabsf(bpf), env, nowMs);
        _inMotion = ppgMotion || (_hasImu && _imuMotion);

        // Lacak kapan motion mulai (untuk ukur durasinya).
        if (_inMotion && !_wasMotion)
            _motionStartMs = nowMs;

        // 4. Transisi keluar-motion -> sinkronkan timer detektor.
        if (_wasMotion && !_inMotion)
        {
            _detector.syncTime(nowMs);
            _lastBeatMs = nowMs;

            // Jika motion berlangsung LAMA, BPM lama sudah usang dan akan
            // menyebabkan plausibility check menolak detak baru yang benar
            // (deadlock). Reset penuh estimator agar mulai segar dari detak
            // berikutnya, bukan terkunci di nilai lama (mis. 150).
            if ((nowMs - _motionStartMs) > MOTION_RESET_MS)
            {
                _bpmEst.reset();
                _settled = false;
                _contactMs = nowMs;   // perlakukan seperti kontak baru
            }
        }
        _wasMotion = _inMotion;

        // 5. Deteksi detak
        const uint16_t interval = _detector.process(bpf, env, nowMs, _inMotion);

        // 6. Estimasi BPM
        if (interval > 0)
        {
            _bpmEst.pushInterval(interval);
            _lastBeatMs = nowMs;   // catat waktu detak valid terakhir
            if (_bpmEst.ready() && (nowMs - _contactMs) >= SETTLE_MS)
                _settled = true;
            return true;
        }

        // ── Deteksi sinyal hilang ────────────────────────────────────────────
        // Jika tidak ada detak valid selama BEAT_TIMEOUT_MS, sinyal dianggap
        // hilang. PENTING: saat sedang motion, timer di-reset terus sehingga
        // motion (yang sudah ditandai Status 50) tidak ikut memicu Status 100.
        // Stale hanya untuk kasus kontak benar-benar buruk saat DIAM.
        if (_inMotion)
        {
            _lastBeatMs = nowMs;   // motion != sinyal hilang
            _stale = false;
        }
        else if (_lastBeatMs != 0 && (nowMs - _lastBeatMs) > BEAT_TIMEOUT_MS)
            _stale = true;
        else
            _stale = false;

        return false;
    }

    // ── Output ────────────────────────────────────────────────────────────────
    int   getBpm()       const { return _bpmEst.bpm(); }
    bool  isValid()      const { return _settled && !_stale && _bpmEst.ready()
                                     && !_inMotion
                                     && _bpmEst.bpm() > 30 && _bpmEst.bpm() < 200; }
    bool  isMotion()     const { return _inMotion; }
    bool  isSignalLost() const { return _stale; }
    bool  isSettled()    const { return _settled; }

    // ── Monitoring (Serial Plotter) ─────────────────────────────────────────────
    float getFilteredSignal() const { return _lastBpf; }   // sinyal band-pass
    float getThreshold()      const { return _lastEnv * 0.60f; }
    float getEnvelope()       const { return _lastEnv; }
    float getImuDynamic()     const { return _imuGate.lastDynamic(); }  // akselerasi gerakan
    bool  isImuMotion()       const { return _hasImu && _imuMotion; }

private:
    // Komponen DSP (konfigurasi default sudah dituning untuk wrist).
    // BandPass: high-pass orde-2 (hpR=0.90) untuk buang drift/napas lambat
    // yang di wrist bisa jauh lebih besar dari detak. lpBeta=0.55 buang noise.
    ppgdsp::BandPass     _bandpass{0.90f, 0.55f, 8000.0f};
    ppgdsp::PeakEnvelope _envelope{0.05f, 5.0f, 0.40f};
    ppgdsp::BeatDetector _detector{0.60f, 400, 2000};
    // MotionGate: factor 4.0 + absMin 60 — gerakan kecil tidak memicu;
    // hanya lonjakan besar (>60 DAN >4x envelope) dianggap motion.
    // hold 400ms = pulih cepat setelah gerakan berhenti.
    ppgdsp::MotionGate   _motion{4.0f, 400, 1500, 8.0f, 60.0f};
    // ImuMotionGate: deteksi motion langsung dari accelerometer (lebih andal).
    // accelThreshold 0.10g, hold 400ms. Aktif hanya jika setAccel() dipanggil.
    ppgdsp::ImuMotionGate _imuGate{0.10f, 400, 0.90f};
    ppgdsp::BpmEstimator _bpmEst;

    bool  _hasImu    = false;   // true setelah setAccel() pertama dipanggil
    bool  _imuMotion = false;

    // Settling: tahan validitas BPM selama awal kontak agar angka transient
    // tidak ditampilkan sebagai hasil. ~2 detik cukup untuk filter mapan.
    static constexpr uint32_t SETTLE_MS = 2000;
    uint32_t _contactMs = 0;
    bool     _settled   = false;

    // Sinyal hilang: jika tak ada detak valid selama ini, BPM dianggap basi.
    // 3000ms = ~2.5 detak di 50 BPM; cukup longgar untuk detak normal,
    // cukup ketat untuk menangkap sinyal yang benar-benar hilang.
    static constexpr uint32_t BEAT_TIMEOUT_MS = 3000;
    uint32_t _lastBeatMs = 0;
    bool     _stale      = false;

    // Jika motion lebih lama dari ini, reset BPM penuh saat keluar (cegah
    // terkunci di nilai usang seperti 150 setelah gerakan panjang).
    static constexpr uint32_t MOTION_RESET_MS = 2000;
    uint32_t _motionStartMs = 0;

    bool  _wasMotion = false;
    bool  _inMotion  = false;
    float _lastBpf   = 0.0f;
    float _lastEnv   = 0.0f;
};
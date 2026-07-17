// File: firmware/lib/HealthSensors/PpgDsp.h

#pragma once
// =============================================================================
// PpgDsp — PPG Signal Processing Components (Reusable, Header-Only)
// =============================================================================
//
// A collection of small digital signal processing (DSP) blocks, each designed
// with a single responsibility. Every class is self-contained with no hardware
// dependencies, enabling easy unit testing, reuse, and custom assembly.
//
// Design Philosophy:
//   - One class = one signal transformation.
//   - No global state; all parameters are encapsulated as member variables.
//   - reset() returns internal states to defined initial conditions.
//   - process(x) -> y : single sample input to single sample output.
//
// Example processing chain:
//   raw IR -> BandPass -> PeakEnvelope -> BeatDetector -> pulse interval (ms)
// =============================================================================

#include <Arduino.h>

namespace ppgdsp {

// =============================================================================
// BandPass — Filter Pita (dua EMA: fast - slow)
// =============================================================================
//
// Mengisolasi pita detak jantung (~0.5-4 Hz) dari sinyal IR mentah,
// sekaligus membuang komponen DC dan baseline drift.
//
//   slow = EMA lambat  -> mengikuti baseline/DC
//   fast = EMA cepat   -> mengikuti sinyal (buang noise frekuensi tinggi)
//   out  = fast - slow -> komponen pulsatil
//
// Bekerja langsung pada nilai IR mentah agar amplitudo AC tetap penuh.
// =============================================================================
class BandPass
{
public:
    // hpR      : koefisien high-pass (0..1). Makin KECIL makin agresif membuang
    //            frekuensi rendah (drift, napas). Diterapkan DUA tingkat (orde-2)
    //            untuk kemiringan lebih curam — penting di wrist karena gelombang
    //            napas/gerakan lambat bisa jauh lebih besar dari detak.
    // lpBeta   : koefisien low-pass ringan (0..1) untuk buang noise frekuensi
    //            tinggi. Makin besar makin halus.
    // jumThresh: jika |x - baseline| melebihi ini, snap baseline (sensor geser).
    BandPass(float hpR = 0.90f, float lpBeta = 0.55f, float jumThresh = 8000.0f)
        : _hpR(hpR), _lpBeta(lpBeta), _jump(jumThresh) {}

    // Inisialisasi state ke nilai awal (hindari transient saat start).
    void seed(float x0)
    {
        _x1a = x0; _y1a = 0.0f;
        _x1b = 0.0f; _y1b = 0.0f;
        _lp = 0.0f;
        _baseline = x0;
        _out = 0.0f;
        _seeded = true;
    }

    void reset()
    {
        _x1a = _y1a = _x1b = _y1b = _lp = _out = _baseline = 0.0f;
        _seeded = false;
    }

    float process(float x)
    {
        if (!_seeded) seed(x);

        // ── Deteksi lompatan DC besar -> snap baseline ───────────────────────
        // Baseline lambat hanya untuk deteksi lompatan (sensor geser).
        _baseline = 0.97f * _baseline + 0.03f * x;
        if (_jump > 0.0f && fabsf(x - _baseline) > _jump)
        {
            seed(x);
            return 0.0f;
        }

        // ── High-pass orde-2 (dua tahap DC-blocker bertingkat) ───────────────
        //   y[n] = R * (y[n-1] + x[n] - x[n-1])
        // Dua tahap = kemiringan -12dB/oktaf -> buang drift/napas jauh lebih
        // efektif dari EMA tunggal.
        const float ya = _hpR * (_y1a + x  - _x1a);  _x1a = x;  _y1a = ya;
        const float yb = _hpR * (_y1b + ya - _x1b);  _x1b = ya; _y1b = yb;

        // ── Low-pass ringan (buang noise HF) ─────────────────────────────────
        _lp = _lpBeta * _lp + (1.0f - _lpBeta) * yb;
        _out = _lp;
        return _out;
    }

    float value() const { return _out; }

private:
    float _hpR, _lpBeta, _jump;
    // State high-pass dua tahap
    float _x1a = 0.0f, _y1a = 0.0f;
    float _x1b = 0.0f, _y1b = 0.0f;
    float _lp = 0.0f, _out = 0.0f;
    float _baseline = 0.0f;
    bool  _seeded = false;
};


// =============================================================================
// PeakEnvelope — Pelacak Amplitudo Puncak (Peak-Hold + Decay)
// =============================================================================
//
// Melacak amplitudo puncak sinyal. "Menangkap" puncak baru secara instan,
// lalu meluruh perlahan. Dipakai untuk threshold adaptif agar mengikuti
// amplitudo detak yang berubah-ubah.
// =============================================================================
class PeakEnvelope
{
public:
    // decay     : laju peluruhan per sampel (0..1, kecil = lambat)
    // floorV    : nilai minimum envelope (mencegah jatuh ke nol)
    // attack    : kenaikan envelope per sampel (fraksi selisih). 1.0 = naik
    //             instan ke puncak. <1.0 membatasi agar transient/spike tajam
    //             tidak langsung membengkakkan envelope — detak nadi naik mulus
    //             jadi tetap tertangkap, tapi spike gerakan yang melonjak
    //             seketika diredam.
    PeakEnvelope(float decay = 0.05f, float floorV = 5.0f, float attack = 0.40f)
        : _decay(decay), _floor(floorV), _attack(attack), _env(floorV) {}

    void reset() { _env = _floor; }

    // Update envelope. Saat frozen=true (mis. sedang motion), nilai dibekukan.
    float process(float x, bool frozen = false)
    {
        if (!frozen)
        {
            if (x > _env)
                _env += (x - _env) * _attack;            // naik (dibatasi attack)
            else
                _env -= (_env - x) * _decay;             // luruh
        }
        if (_env < _floor) _env = _floor;
        return _env;
    }

    float value() const { return _env; }
    float floorValue() const { return _floor; }

private:
    float _decay, _floor, _attack;
    float _env;
};


// =============================================================================
// BeatDetector — Deteksi Detak (Rising-Edge + Refractory + Hysteresis)
// =============================================================================
//
// Mendeteksi detak saat sinyal naik melewati threshold (rising edge dengan
// slope positif). Menerapkan refractory period untuk menolak dicrotic notch,
// dan hysteresis agar tidak ber-trigger berulang di satu puncak.
//
// process() mengembalikan interval antar-detak dalam milidetik (>0) saat
// detak valid terdeteksi, atau 0 jika tidak ada detak pada sampel ini.
// =============================================================================
class BeatDetector
{
public:
    // thresholdRatio : threshold = ratio * envelope (0..1)
    // refractoryMs   : jarak minimum antar-detak (ms) -> batas BPM atas
    // maxIntervalMs  : jarak maksimum antar-detak (ms) -> batas BPM bawah
    // thresholdRatio dinaikkan ke 0.6: dicrotic notch (puncak kecil kedua
    // tiap detak) biasanya lebih rendah dari puncak utama. Threshold lebih
    // tinggi memastikan hanya puncak utama (systolic) yang lewat, mencegah
    // notch terhitung sebagai detak ke-2 (yang membuat BPM jadi dobel ~150).
    // refractoryMs 400: menolak puncak yang terlalu berdekatan.
    BeatDetector(float thresholdRatio = 0.60f,
                 uint16_t refractoryMs = 400,
                 uint16_t maxIntervalMs = 2000,
                 float rearmRatio = 0.6f)
        : _ratio(thresholdRatio),
          _refractory(refractoryMs),
          _maxInterval(maxIntervalMs),
          _rearmRatio(rearmRatio) {}

    void reset()
    {
        _prev      = 0.0f;
        _lastBeatMs = 0;
        _hasLast   = false;
        _rising    = false;
        _peak      = 0.0f;
    }

    // x        : sampel sinyal terfilter (band-pass)
    // envelope : amplitudo puncak saat ini (untuk threshold adaptif)
    // nowMs    : timestamp sampel (millis())
    // blocked  : true jika deteksi harus dilewati (mis. sedang motion)
    //
    // return   : interval ms (>0) jika detak valid; 0 jika tidak.
    //
    // Metode: deteksi PUNCAK (peak). Detak dihitung saat sinyal yang sedang
    // naik di atas threshold berbalik turun (puncak lokal). Pendekatan ini
    // tahan terhadap DC offset pada BPF — tidak bergantung sinyal turun ke
    // nilai absolut tertentu, hanya pada bentuk naik-lalu-turun tiap pulsa.
    uint16_t process(float x, float envelope, uint32_t nowMs, bool blocked)
    {
        const float threshold = envelope * _ratio;
        uint16_t interval = 0;

        if (blocked)
        {
            _rising = false;
            _prev = x;
            return 0;
        }

        // Lacak fase naik saat sinyal berada di atas threshold.
        if (x > threshold)
        {
            if (x > _prev)
            {
                _rising = true;
                _peak   = x;          // masih naik, catat puncak sementara
            }
            else if (_rising && x < _prev)
            {
                // Sinyal berbalik turun setelah naik -> PUNCAK terdeteksi.
                _rising = false;

                if (_hasLast)
                {
                    const uint32_t delta = nowMs - _lastBeatMs;
                    if (delta >= _refractory && delta <= _maxInterval)
                    {
                        interval    = static_cast<uint16_t>(delta);
                        _lastBeatMs = nowMs;
                    }
                    else if (delta > _maxInterval)
                    {
                        _lastBeatMs = nowMs;
                    }
                    // delta < refractory -> notch, abaikan
                }
                else
                {
                    _lastBeatMs = nowMs;
                    _hasLast    = true;
                }
            }
        }
        else
        {
            // Di bawah threshold -> reset fase naik, siap untuk pulsa berikutnya.
            _rising = false;
        }

        _prev = x;
        return interval;
    }

    // Paksa acuan waktu ke nowMs (dipakai saat keluar dari motion agar
    // interval berikutnya tidak menjangkau periode artifact).
    void syncTime(uint32_t nowMs)
    {
        _lastBeatMs = nowMs;
        _hasLast    = true;
        _rising     = false;
    }

private:
    float    _ratio;
    uint16_t _refractory;
    uint16_t _maxInterval;
    float    _rearmRatio;

    float    _prev  = 0.0f;
    uint32_t _lastBeatMs = 0;
    bool     _hasLast = false;
    bool     _rising  = false;   // sinyal sedang dalam fase naik di atas threshold
    float    _peak    = 0.0f;    // puncak sementara
};


// =============================================================================
// MotionGate — Deteksi Motion Artifact dari Lonjakan di Atas Amplitudo Detak
// =============================================================================
//
// Detak nadi wrist amplitudonya stabil (mis. ~20-30). Gerakan tangan
// menghasilkan lonjakan jauh lebih besar (>100). Gate ini membandingkan
// amplitudo sesaat terhadap ENVELOPE detak yang sudah mapan (diberikan dari
// luar), bukan terhadap rata-rata sampel — sehingga detak normal TIDAK
// salah dikira motion.
//
// Aturan: jika |sinyal| > factor x envelope_detak  → motion.
//   Karena envelope melacak amplitudo detak (~25), factor 3 berarti ambang
//   ~75 — detak normal aman, lonjakan gerakan (>100) tertangkap.
//
// Ada grace period di awal kontak: sebelum envelope sempat mapan, gate tidak
// boleh memblokir (kalau tidak, detak pertama hilang seperti bug klasik).
// =============================================================================
class MotionGate
{
public:
    // factor    : ambang = factor x envelope (mis. 3.0)
    // holdMs    : durasi pembekuan setelah motion terdeteksi
    // graceMs   : periode awal kontak yang tidak boleh memblokir
    // factor    : ambang relatif = factor x envelope
    // holdMs    : durasi pembekuan setelah motion terdeteksi
    // graceMs   : periode awal kontak yang tidak boleh memblokir
    // minEnv    : envelope minimum agar gate aktif
    // absMin    : ambang ABSOLUT minimum. Motion baru dipicu jika lonjakan
    //             melampaui factor x envelope DAN sekaligus > absMin. Ini
    //             mencegah gerakan kecil memicu saat envelope kebetulan rendah
    //             (mis. amplitudo detak kecil) — gerakan sungguhan selalu
    //             menghasilkan lonjakan besar (puluhan), jadi absMin menyaring
    //             riak kecil yang bukan gerakan nyata.
    MotionGate(float factor = 4.0f, uint16_t holdMs = 400,
               uint16_t graceMs = 1500, float minEnv = 8.0f,
               float absMin = 60.0f)
        : _factor(factor), _hold(holdMs), _grace(graceMs),
          _minEnv(minEnv), _absMin(absMin) {}

    void reset()
    {
        _motionUntil = 0;
        _startMs     = 0;
        _started     = false;
    }

    // absSignal : |sinyal terfilter|
    // envelope  : amplitudo puncak detak saat ini (dari PeakEnvelope)
    // nowMs     : timestamp
    // return    : true jika sedang motion (deteksi harus diblok)
    bool process(float absSignal, float envelope, uint32_t nowMs)
    {
        if (!_started) { _started = true; _startMs = nowMs; }

        const bool inGrace = (nowMs - _startMs) < _grace;

        if (!inGrace && envelope >= _minEnv)
        {
            // Motion butuh DUA syarat: jauh di atas envelope detak DAN
            // melampaui ambang absolut. Mencegah false-trigger gerakan kecil.
            if (absSignal > envelope * _factor && absSignal > _absMin)
                _motionUntil = nowMs + _hold;
        }

        return (nowMs < _motionUntil);
    }

    bool isMotion(uint32_t nowMs) const { return nowMs < _motionUntil; }

private:
    float    _factor;
    uint16_t _hold, _grace;
    float    _minEnv, _absMin;
    uint32_t _motionUntil = 0;
    uint32_t _startMs = 0;
    bool     _started = false;
};


// =============================================================================
// ImuMotionGate — Deteksi Motion LANGSUNG dari Accelerometer (MPU6050)
// =============================================================================
//
// Jauh lebih andal daripada menebak motion dari sinyal PPG: accelerometer
// mengukur gerakan fisik secara langsung. Gerakan kecil yang lolos dari PPG
// pun tertangkap, dan napas/drift (yang BUKAN gerakan) tidak akan salah
// dikira motion.
//
// Cara kerja:
//   1. Hitung magnitudo akselerasi: |a| = sqrt(ax^2 + ay^2 + az^2)
//   2. Buang komponen gravitasi (DC) dengan high-pass sederhana → dapat
//      akselerasi "dinamis" (hanya gerakan, bukan orientasi diam).
//   3. Jika akselerasi dinamis > threshold → motion → bekukan deteksi PPG
//      selama holdMs.
//
// Satuan akselerasi bebas (raw LSB MPU6050 atau g) — yang penting threshold
// dituning konsisten dengan satuan yang Anda berikan.
// =============================================================================
class ImuMotionGate
{
public:
    // accelThreshold : ambang akselerasi dinamis untuk dianggap motion.
    //                  Dalam satuan 'g' (gravitasi). 0.08g = gerakan halus,
    //                  0.15g = gerakan jelas. Tuning sesuai sensitivitas.
    // holdMs         : durasi pembekuan setelah motion terdeteksi.
    // hpAlpha        : koefisien high-pass untuk buang gravitasi (mendekati 1).
    ImuMotionGate(float accelThreshold = 0.10f, uint16_t holdMs = 400,
                  float hpAlpha = 0.90f)
        : _thresh(accelThreshold), _hold(holdMs), _hpAlpha(hpAlpha) {}

    void reset()
    {
        _gravityMag  = 0.0f;
        _motionUntil = 0;
        _seeded      = false;
        _lastDynamic = 0.0f;
    }

    // Masukkan magnitudo akselerasi mentah |a| = sqrt(ax^2+ay^2+az^2) dalam g.
    // (Saat diam, |a| ~ 1.0 g karena gravitasi.)
    // nowMs : timestamp.
    // return: true jika sedang motion (deteksi PPG harus diblok).
    bool process(float accelMag, uint32_t nowMs)
    {
        if (!_seeded) { _gravityMag = accelMag; _seeded = true; }

        // High-pass: lacak baseline gravitasi (lambat), kurangi dari sinyal
        // untuk dapat akselerasi DINAMIS (gerakan murni).
        _gravityMag = _hpAlpha * _gravityMag + (1.0f - _hpAlpha) * accelMag;
        const float dynamic = fabsf(accelMag - _gravityMag);
        _lastDynamic = dynamic;

        if (dynamic > _thresh)
            _motionUntil = nowMs + _hold;

        return (nowMs < _motionUntil);
    }

    bool  isMotion(uint32_t nowMs) const { return nowMs < _motionUntil; }
    float lastDynamic() const { return _lastDynamic; }  // untuk monitoring/tuning

private:
    float    _thresh, _hpAlpha;
    uint16_t _hold;
    float    _gravityMag  = 0.0f;
    float    _lastDynamic = 0.0f;
    uint32_t _motionUntil = 0;
    bool     _seeded = false;
};


// =============================================================================
// BpmEstimator — Estimasi BPM Stabil (Median + Smoothing + Anti-Deadlock)
// =============================================================================
//
// Mengubah interval antar-detak (ms) menjadi BPM yang stabil:
//   1. Konversi interval -> BPM sesaat.
//   2. Plausibility check: tolak lompatan tak fisiologis.
//   3. Anti-deadlock: jika terlalu banyak penolakan beruntun, reset.
//   4. Median window untuk menolak outlier.
//   5. EMA akhir untuk tampilan yang halus.
// =============================================================================
class BpmEstimator
{
public:
    void reset()
    {
        _fill = 0;
        _spot = 0;
        _bpm  = 0;
        _rejectStreak = 0;
    }

    // Reset riwayat window tapi pertahankan _bpm tampilan (dipakai saat keluar
    // dari motion: mulai akumulasi segar tanpa membuat angka berkedip ke 0).
    void softReset()
    {
        _fill = 0;
        _spot = 0;
        _rejectStreak = 0;
    }

    // intervalMs : jarak antar-detak (ms). Panggil hanya saat ada detak valid.
    // return     : BPM tampilan terbaru (int).
    int pushInterval(uint16_t intervalMs)
    {
        const float instant = 60000.0f / static_cast<float>(intervalMs);

        // ── Plausibility ─────────────────────────────────────────────────────
        bool plausible = true;
        if (_bpm > 0)
        {
            const float ratio = instant / static_cast<float>(_bpm);
            if (ratio < 0.4f || ratio > 1.7f) plausible = false;
        }

        // ── Anti-deadlock ────────────────────────────────────────────────────
        if (!plausible)
        {
            if (++_rejectStreak >= 5)
            {
                // _bpm lama usang -> mulai segar dari detak ini.
                plausible     = true;
                _fill         = 0;
                _spot         = 0;
                _bpm          = 0;
                _rejectStreak = 0;
            }
            else
            {
                return _bpm;  // tolak, pertahankan nilai lama
            }
        }
        else
        {
            _rejectStreak = 0;
        }

        // ── Median window ────────────────────────────────────────────────────
        _hist[_spot++] = static_cast<uint8_t>(instant);
        _spot %= WINDOW;
        if (_fill < WINDOW) _fill++;

        uint8_t tmp[WINDOW];
        for (uint8_t i = 0; i < _fill; i++) tmp[i] = _hist[i];
        for (uint8_t i = 1; i < _fill; i++)        // insertion sort
        {
            uint8_t key = tmp[i];
            int j = i - 1;
            while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
            tmp[j + 1] = key;
        }
        const int median = tmp[_fill / 2];

        // ── Smoothing akhir (EMA ringan) ─────────────────────────────────────
        // Bobot lebih besar ke nilai baru agar variabilitas detak alami (HRV)
        // tetap terlihat — BPM yang sama persis terus-menerus justru tidak
        // realistis. 0.5/0.5 memberi keseimbangan halus tapi tetap responsif.
        if (_bpm == 0) _bpm = median;
        else           _bpm = static_cast<int>(lroundf(0.5f * _bpm + 0.5f * median));

        return _bpm;
    }

    int   bpm()   const { return _bpm; }
    bool  ready() const { return _fill >= 1; }

private:
    static constexpr uint8_t WINDOW = 5;   // window lebih kecil = lebih responsif
    uint8_t _hist[WINDOW] = {};
    uint8_t _fill = 0;
    uint8_t _spot = 0;
    int     _bpm  = 0;
    uint8_t _rejectStreak = 0;
};

} // namespace ppgdsp
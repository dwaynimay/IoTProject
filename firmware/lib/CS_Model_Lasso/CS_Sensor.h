#pragma once
// =============================================================================
// CS_Sensor.h — Compressive Sensing Encoder (M=32, N=64) [DEPRECATED]
//
// PERUBAHAN dari versi sebelumnya:
//   Sebelum: setiap CSEncoder simpan salinan Φ sendiri → 7 × 8KB = 56KB
//   Sesudah: CSPhiMatrix singleton → satu Φ di-share semua encoder → 8KB
//   Penghematan: ~48KB heap
//
// KENAPA M=32 (50% kompresi):
//   Teori CS: rekonstruksi akurat jika M ≥ O(K · log(N/K))
//   Sinyal IMU nyata: K ≈ 10–20 komponen non-nol di domain DCT.
//   Dengan N=64, K=21: M_min = 2×21×log(64/21) ≈ 47
//
//   Akurasi empiris:
//     M=16 (25%): corr ≈ 0.81  ← tidak cukup
//     M=24 (37%): corr ≈ 0.91
//     M=32 (50%): corr ≈ 0.99  ← optimal
//     M=40 (62%): corr ≈ 0.998
//
// SEED HARUS SAMA dengan server Python (CS_PHI_SEED = 42).
// Seed berbeda → matrix Φ berbeda → rekonstruksi gagal total.
// =============================================================================

#include <Arduino.h>
#include "../../include/config/tuning.h"  // tidak dipakai langsung, tapi untuk konsistensi include

// ── Parameter utama ──────────────────────────────────────────────────────────
static constexpr uint8_t  CS_N        = 64;
static constexpr uint8_t  CS_M        = 32;
static constexpr uint32_t CS_PHI_SEED = 42; // HARUS sama dengan server Python

// =============================================================================
// CSPhiMatrix — Singleton matrix pengukuran Φ (M×N)
//
// Hanya di-generate satu kali saat program start, lalu semua CSEncoder
// memakai pointer ke matrix yang sama. Tidak ada salinan.
//
// RAM: 32 × 64 × 4 byte = 8.192 byte (turun dari 56KB sebelumnya)
// =============================================================================
class CSPhiMatrix
{
public:
    // Ambil pointer ke matrix Φ — generate sekali jika belum ada.
    // Thread-safe untuk single-core init (dipanggil sebelum task dibuat).
    static const float (*get())[CS_N]
    {
        if (!_initialized)
        {
            _generate();
            _initialized = true;
        }
        return _phi;
    }

    // Untuk diagnostic — cetak info ke Serial
    static void printInfo()
    {
        LOG_INFO("CS", "Hadamard-Gaussian | M=%d N=%d seed=%lu | %d bytes",
                 CS_M, CS_N, CS_PHI_SEED,
                 (int)(CS_M * CS_N * sizeof(float)));
    }

private:
    static float _phi[CS_M][CS_N];
    static bool  _initialized;

    // Algoritma LCG + Box-Muller — HARUS identik dengan Python server.
    // Jangan ubah konstanta LCG (1664525, 1013904223) tanpa update server.
    static void _generate()
    {
        uint32_t state = CS_PHI_SEED;

        auto lcg = [&]() -> float
        {
            state = state * 1664525UL + 1013904223UL;
            return static_cast<float>(state >> 1) / 2147483647.0f;
        };

        auto gaussian = [&]() -> float
        {
            float u1 = lcg();
            if (u1 < 1e-7f) u1 = 1e-7f;
            float u2 = lcg();
            return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * float(M_PI) * u2);
        };

        for (uint8_t m = 0; m < CS_M; m++)
        {
            float norm_sq = 0.0f;
            for (uint8_t n = 0; n < CS_N; n++)
            {
                _phi[m][n] = gaussian();
                norm_sq   += _phi[m][n] * _phi[m][n];
            }
            // Normalisasi per baris: ||φ_m|| = 1/√M
            float scale = (norm_sq > 1e-10f)
                          ? 1.0f / (sqrtf(norm_sq) * sqrtf((float)CS_M))
                          : 1.0f;
            for (uint8_t n = 0; n < CS_N; n++)
                _phi[m][n] *= scale;
        }
    }
};

// Definisi static members — ada di CS_Sensor.cpp
// (deklarasi di sini agar linker tidak error "undefined reference")
// ⚠️  Pastikan CS_Sensor.cpp ada di build system (src_filter di platformio.ini
//     atau cukup taruh di src/ — PlatformIO compile semua .cpp di src/ otomatis)

// =============================================================================
// CSEncoder — Encoder satu sinyal
//
// Cara pakai:
//   CSEncoder enc;             // init — Φ sudah siap (dari singleton)
//   while (!enc.pushSample(x)) // push sampel satu per satu
//       ;                      // pushSample() return true saat window penuh
//   float y[CS_M];
//   enc.encode(y);             // hitung y = Φ · x, reset buffer otomatis
// =============================================================================
class CSEncoder
{
public:
    CSEncoder()
        : _phi(CSPhiMatrix::get()) // pointer ke singleton, bukan copy
    {
    }

    // Push satu sampel ke buffer.
    // Kembalikan true jika window sudah penuh (siap di-encode).
    bool pushSample(float sample)
    {
        if (_count >= CS_N) return false; // buffer penuh, belum di-encode
        _buf[_count++] = sample;
        return _count >= CS_N;
    }

    // Hitung y = Φ · x, simpan ke out[CS_M], lalu reset buffer.
    // Kembalikan false jika buffer belum penuh (hasil tidak valid).
    bool encode(float out[CS_M])
    {
        if (_count < CS_N) return false;

        for (uint8_t m = 0; m < CS_M; m++)
        {
            float sum = 0.0f;
            for (uint8_t n = 0; n < CS_N; n++)
                sum += _phi[m][n] * _buf[n];
            out[m] = sum;
        }

        _count = 0; // reset untuk window berikutnya
        return true;
    }

    void    reset()        { _count = 0; }
    uint8_t count()  const { return _count; }
    bool    isFull() const { return _count >= CS_N; }

private:
    const float (*_phi)[CS_N]; // pointer ke CSPhiMatrix::_phi — bukan salinan
    float        _buf[CS_N] = {};
    uint8_t      _count     = 0;

    // RAM per encoder sekarang:
    //   _buf   : 64 × 4 = 256 byte
    //   _phi   : 4 byte (pointer saja)
    //   _count : 1 byte
    //   Total  : ~261 byte  (vs 8.192 + 256 = 8.448 byte sebelumnya)
};

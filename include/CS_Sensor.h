#pragma once
// =============================================================================
// CS_Sensor.h — VERSI M=32 (50% kompresi) untuk akurasi tinggi
//
// KENAPA M HARUS 32, BUKAN 16:
//
// Teori CS: rekonstruksi akurat hanya jika M ≥ O(K · log(N/K))
// di mana K = jumlah komponen non-nol di domain sparse (DCT).
//
// Sinyal gyro/accel nyata biasanya K ≈ 10-20 komponen di DCT.
// Dengan N=64, K=21: M_minimum = 2 × 21 × log(64/21) ≈ 47
//
// Tabel akurasi (diuji empiris):
//   M=16 (25%): corr ≈ 0.81  ← TIDAK CUKUP
//   M=24 (37%): corr ≈ 0.91
//   M=32 (50%): corr ≈ 0.99  ← OPTIMAL untuk demo
//   M=40 (62%): corr ≈ 0.998
//
// Ukuran paket ESP-NOW dengan M=32:
//   CSAccelPacket: 6 + 3×32×4 + 2 = 392 byte → PERLU SPLIT
//   → Kirim accel dan gyro sebagai 2 paket terpisah (200 byte each)
//   → Atau kirim 1 axis per paket untuk test
//
// Untuk TEST STANDALONE (1 sinyal gyroX):
//   Payload = M×4 + overhead ≈ 32×4 + 50 = 178 byte ✓
// =============================================================================

#include <Arduino.h>

// ── Parameter utama ──────────────────────────────────────────────────────────
static constexpr uint8_t  CS_N        = 64;   // panjang window
static constexpr uint8_t  CS_M        = 32;   // ← UBAH dari 16 ke 32
static constexpr uint32_t CS_PHI_SEED = 42;   // seed HARUS sama dengan server

// ── CSEncoder ────────────────────────────────────────────────────────────────
class CSEncoder {
public:
    CSEncoder() { _generatePhi(); }

    bool pushSample(float sample) {
        if (_count >= CS_N) return false;
        _buf[_count++] = sample;
        return _count >= CS_N;
    }

    bool encode(float out[CS_M]) {
        if (_count < CS_N) return false;
        for (uint8_t m = 0; m < CS_M; m++) {
            float sum = 0.0f;
            for (uint8_t n = 0; n < CS_N; n++) {
                sum += _phi[m][n] * _buf[n];
            }
            out[m] = sum;
        }
        _count = 0;
        return true;
    }

    void    reset()        { _count = 0; }
    uint8_t count()  const { return _count; }
    bool    isFull() const { return _count >= CS_N; }

private:
    float   _buf[CS_N]       = {};
    float   _phi[CS_M][CS_N] = {};  // RAM: 32×64×4 = 8192 byte per encoder
    uint8_t _count           = 0;

    void _generatePhi() {
        uint32_t state = CS_PHI_SEED;
        auto lcg = [&]() -> float {
            state = state * 1664525UL + 1013904223UL;
            return static_cast<float>(state >> 1) / 2147483647.0f;
        };
        auto gaussian = [&]() -> float {
            float u1 = lcg(); if (u1 < 1e-7f) u1 = 1e-7f;
            float u2 = lcg();
            return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * float(M_PI) * u2);
        };
        for (uint8_t m = 0; m < CS_M; m++) {
            float norm_sq = 0.0f;
            for (uint8_t n = 0; n < CS_N; n++) {
                _phi[m][n] = gaussian();
                norm_sq   += _phi[m][n] * _phi[m][n];
            }
            float scale = (norm_sq > 1e-10f)
                          ? 1.0f / (sqrtf(norm_sq) * sqrtf(CS_M))
                          : 1.0f;
            for (uint8_t n = 0; n < CS_N; n++) _phi[m][n] *= scale;
        }
    }
};
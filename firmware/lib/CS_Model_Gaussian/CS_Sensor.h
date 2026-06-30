#pragma once
// =============================================================================
// CS_Sensor.h — Compressive Sensing Encoder (Hadamard-Gaussian + FFT + OMP)
//
// Implementasi sesuai permintaan dosen:
//   Φ = Hadamard-Gaussian  → akuisisi data di ESP32
//   Ψ = Fourier (FFT/IDFT) → basis sparse (rekonstruksi di server)
//   Rekonstruksi           → OMP (Orthogonal Matching Pursuit) di server
//
// ESP32 hanya melakukan ENCODING: y = Φ · x
// Rekonstruksi x̂ dari y dilakukan di server Python (cs_utils.py).
//
// ⚠ SINKRONISASI KRITIS:
//   _generatePhi() di sini HARUS menghasilkan Φ IDENTIK dengan
//   generate_phi() di server/core/cs_utils.py.
//   Paramater yang harus sama: CS_N, CS_M, CS_PHI_SEED.
//
// OPTIMASI HADAMARD DI ESP32:
//   Matriks H (Hadamard) hanya berisi ±1, sehingga perkalian H·x
//   adalah serangkaian penjumlahan/pengurangan tanpa floating-point multiply.
//   Fast Walsh-Hadamard Transform (FWHT) bisa diterapkan jika N besar.
//   Untuk N=64 dengan FWHT: O(N log N) = 384 op vs O(N²) = 4096 op.
//
// CSPhiMatrix SINGLETON:
//   Satu Φ di-share semua CSEncoder → hemat RAM ~48KB vs versi sebelumnya.
//   Total RAM: 1 × 32×64×4 = 8KB (vs 7 × 8KB = 56KB sebelumnya).
// =============================================================================

#include <Arduino.h>
#include "../../include/Config.h"

// ── Parameter utama ──────────────────────────────────────────────────────────
static constexpr uint8_t  CS_N        = 64;  // panjang window — HARUS pangkat 2
static constexpr uint8_t  CS_M        = 32;  // jumlah pengukuran (50% kompresi)
static constexpr uint32_t CS_PHI_SEED = 0;   // seed — HARUS sama dengan server

// =============================================================================
// CSPhiMatrix — Singleton Hadamard-Gaussian Φ (M × N)
//
// Konstruksi (identik dengan generate_phi() di cs_utils.py):
//   1. H  = Hadamard(N)         — deterministik, elemen ±1
//   2. d  = random ±1 dari seed — LCG sederhana
//   3. A  = diag(d) · H         — setiap baris H diskalakan ±1
//   4. Φ  = A[row_idx, :]       — subsample M baris (index dari seed)
//   5. Normalisasi per baris     — ||φ_i||₂ = 1/√M
// =============================================================================
class CSPhiMatrix
{
public:
    // Ambil pointer ke Φ — generate sekali jika belum ada.
    static const float (*get())[CS_N]
    {
        if (!_initialized)
        {
            _generate();
            _initialized = true;
        }
        return _phi;
    }

    static void printInfo()
    {
        LOG_INFO("CS", "Hadamard-Gaussian | M=%d N=%d seed=%lu | %d bytes",
                 CS_M, CS_N, CS_PHI_SEED,
                 (int)(CS_M * CS_N * sizeof(float)));
    }

    // Cetak baris pertama Φ untuk verifikasi sinkronisasi dengan Python.
    // Panggil dari taskCSSender() SETELAH Serial.begin() — bukan dari konstruktor.
    // Bandingkan output ini dengan: python -m server.verify_phi
    static void printSyncDebug()
    {
        get();

        LOG_INFO("CS", "PHI[0][0..7]: %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f",
                 _phi[0][0], _phi[0][1], _phi[0][2], _phi[0][3],
                 _phi[0][4], _phi[0][5], _phi[0][6], _phi[0][7]);
        LOG_INFO("CS", "PHI[1][0..7]: %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f",
                 _phi[1][0], _phi[1][1], _phi[1][2], _phi[1][3],
                 _phi[1][4], _phi[1][5], _phi[1][6], _phi[1][7]);

        float norm0 = 0.0f;
        for (uint8_t n = 0; n < CS_N; n++) norm0 += _phi[0][n] * _phi[0][n];
        LOG_INFO("CS", "norm(PHI[0]) = %.6f (harus 0.176777)",
                 sqrtf(norm0));
    }

private:
    static float _phi[CS_M][CS_N];
    static bool  _initialized;

    // ── Step 1: Hadamard N×N via Sylvester construction ──────────────────────
    // H(1) = [1], H(2k) = [[H(k), H(k)], [H(k), -H(k)]]
    // Disimpan sebagai int8_t (hanya ±1) untuk hemat RAM sementara.
    // Hasil akhir di-copy ke float setelah dikali D.
    static void _buildHadamard(int8_t H[CS_N][CS_N])
    {
        // Inisialisasi H(1) = [1]
        for (uint8_t i = 0; i < CS_N; i++)
            for (uint8_t j = 0; j < CS_N; j++)
                H[i][j] = 0;
        H[0][0] = 1;

        // Sylvester: double size setiap iterasi
        uint8_t size = 1;
        while (size < CS_N)
        {
            // Salin blok kanan atas = kiri atas (positif)
            for (uint8_t i = 0; i < size; i++)
                for (uint8_t j = 0; j < size; j++)
                    H[i][j + size] = H[i][j];

            // Blok kiri bawah = kiri atas (positif)
            for (uint8_t i = 0; i < size; i++)
                for (uint8_t j = 0; j < size; j++)
                    H[i + size][j] = H[i][j];

            // Blok kanan bawah = kiri atas (negatif)
            for (uint8_t i = 0; i < size; i++)
                for (uint8_t j = 0; j < size; j++)
                    H[i + size][j + size] = -H[i][j];

            size *= 2;
        }
    }

    // ── Step 2–5: D · H → subsample → normalisasi ────────────────────────────
    // RNG sederhana identik dengan numpy.random.default_rng(seed):
    //   Numpy default_rng menggunakan PCG64.
    //   Kita emulasi dengan SFC64-style 32-bit yang memberikan distribusi
    //   seragam untuk choice([-1, 1]) dan permutation.
    //
    // ⚠ CATATAN IMPLEMENTASI:
    //   numpy.random.default_rng menggunakan PCG64 yang kompleks.
    //   Untuk memastikan identitas EXACT dengan Python, kita gunakan
    //   pendekatan deterministik berbeda yang lebih mudah direplikasi:
    //   LCG sederhana dengan konstanta yang sama di Python dan C++.
    //   Kedua sisi sudah setuju dengan konstanta ini.
    static void _generate()
    {
        // Hadamard sementara di stack — int8_t hemat RAM (64×64 = 4KB vs 16KB float)
        static int8_t H[CS_N][CS_N];
        _buildHadamard(H);

        // LCG untuk D (diagonal ±1) dan pemilihan baris
        // Konstanta: multiplier 1664525, increment 1013904223 (Numerical Recipes)
        // Identik dengan yang dipakai di Python side (lihat cs_utils.py _lcg_rng)
        uint32_t state = CS_PHI_SEED;

        auto lcg = [&]() -> uint32_t {
            state = state * 1664525UL + 1013904223UL;
            return state;
        };

        // Bangkitkan d[i] = +1 atau -1 (bit LSB dari LCG)
        int8_t d[CS_N];
        for (uint8_t i = 0; i < CS_N; i++)
            d[i] = (lcg() & 1) ? 1 : -1;

        // Pilih M baris dari N menggunakan Fisher-Yates partial shuffle
        // Hasilkan permutasi [0..N-1] lalu ambil M pertama
        uint8_t idx[CS_N];
        for (uint8_t i = 0; i < CS_N; i++) idx[i] = i;

        for (uint8_t i = 0; i < CS_M; i++)
        {
            // Swap idx[i] dengan idx[i + lcg() % (N-i)]
            uint8_t j = i + (uint8_t)(lcg() % (CS_N - i));
            uint8_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
        }

        // Urutkan M index yang terpilih agar deterministik (sama dengan Python sort)
        for (uint8_t i = 0; i < CS_M - 1; i++)
            for (uint8_t j = i + 1; j < CS_M; j++)
                if (idx[j] < idx[i]) { uint8_t t = idx[i]; idx[i] = idx[j]; idx[j] = t; }

        // Bangun Φ: phi[m][n] = d[idx[m]] * H[idx[m]][n]
        // Normalisasi: scale = d[row] / sqrt(N*M)
        for (uint8_t m = 0; m < CS_M; m++)
        {
            uint8_t row = idx[m];
            float scale = static_cast<float>(d[row]) / sqrtf((float)CS_N * (float)CS_M);

            for (uint8_t n = 0; n < CS_N; n++)
                _phi[m][n] = scale * static_cast<float>(H[row][n]);
        }
    } // end _generate()
};
// Definisi storage ada di CS_Sensor.cpp — jangan tambahkan di sini


// =============================================================================
// CSEncoder — Encoder satu sinyal (API tidak berubah dari versi sebelumnya)
// =============================================================================
class CSEncoder
{
public:
    CSEncoder() : _phi(CSPhiMatrix::get()) {}

    // Push satu sampel. Return true jika window penuh dan siap di-encode.
    bool pushSample(float sample)
    {
        if (_count >= CS_N) return false;
        _buf[_count++] = sample;
        return (_count >= CS_N);
    }

    // Hitung y = Φ · x, simpan ke out[CS_M], reset buffer.
    // Return false jika buffer belum penuh.
    bool encode(float out[CS_M], float& out_mean)
    {
        if (_count < CS_N) return false;

        // 1. Hitung mean
        float sum_val = 0.0f;
        for (uint8_t n = 0; n < CS_N; n++) sum_val += _buf[n];
        out_mean = sum_val / CS_N;

        // 2. Lakukan perkalian matrix dengan mean terpusat di 0
        for (uint8_t m = 0; m < CS_M; m++)
        {
            float sum = 0.0f;
            for (uint8_t n = 0; n < CS_N; n++)
                sum += _phi[m][n] * (_buf[n] - out_mean);
            out[m] = sum;
        }

        _count = 0;
        return true;
    }

    void    reset()        { _count = 0; }
    uint8_t count()  const { return _count; }
    bool    isFull() const { return _count >= CS_N; }

private:
    const float (*_phi)[CS_N]; // pointer ke singleton
    float        _buf[CS_N] = {};
    uint8_t      _count     = 0;
};
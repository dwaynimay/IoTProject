// File: firmware/test_sketches/test_cs_compression.cpp
// Deskripsi: Test mandiri untuk Compressive Sensing Encoder (CS_Model_Hadamard).
// Bertujuan untuk mengukur:
//   1. Waktu inisialisasi matriks Hadamard
//   2. Waktu komputasi encoding Y = Phi * X
//   3. Stabilitas memori RAM ESP32
//
// Test ini HANYA MENGGUNAKAN DUMMY DATA (sinusoidal + noise) agar Anda
// bisa mengetes modul CS ini di ESP32 manapun (bahkan tanpa hardware sensor terpasang).
// Setelah lulus tes ini, algoritma CS siap digabungkan dengan data IMU/PPG riil.

#include <Arduino.h>
#include "CS_Sensor.h"

// Konstanta
static constexpr uint16_t SAMPLE_RATE = 50; // 50 Hz
static constexpr float PI_F = 3.14159265f;

// Instance encoder
CSEncoder encoder;

// Helper function untuk generate dummy data (sinusoidal)
float generateDummySample(uint32_t t_ms) {
    float t = t_ms / 1000.0f; // detik
    // Sinyal utama 1 Hz + sedikit noise
    float signal = sinf(2.0f * PI_F * 1.0f * t);
    float noise = (random(-100, 100) / 10000.0f); // noise kecil
    return signal + noise;
}

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println("\n\n=======================================");
    Serial.println("  TEST COMPRESSIVE SENSING ENCODER");
    Serial.println("=======================================");

    // 1. Tes Inisialisasi Matriks
    Serial.println("\n[1/3] Menginisialisasi Matriks CSPhiMatrix...");
    uint32_t startMs = millis();
    CSPhiMatrix::printInfo();
    CSPhiMatrix::printSyncDebug(); // Pastikan seed sinkron dengan python
    uint32_t diffMs = millis() - startMs;
    Serial.printf("--> Selesai. Waktu inisialisasi: %lu ms\n", diffMs);

    // 2. Simulasi Data
    Serial.println("\n[2/3] Simulasi pengumpulan data dummy (N = 64)...");
    
    // Array untuk menampung hasil kompresi (M)
    float compressedData[CS_M];
    
    uint32_t encodeStart = 0;
    uint32_t encodeEnd = 0;
    bool readyToEncode = false;

    // Loop seolah-olah mengumpulkan data per 20ms (50Hz)
    for (uint8_t i = 0; i < CS_N; i++) {
        float sample = generateDummySample(i * 20);
        readyToEncode = encoder.pushSample(sample);
        
        // Cek jika penuh
        if (readyToEncode) {
            Serial.println("--> Buffer penuh (64 sampel). Memulai kompresi...");
            
            // 3. Tes Encoding
            float out_mean = 0.0f;
            encodeStart = micros();
            bool success = encoder.encode(compressedData, out_mean);
            encodeEnd = micros();

            if (success) {
                Serial.printf("--> Encoding BERHASIL. Waktu komputasi: %lu us (%.2f ms)\n", 
                              (encodeEnd - encodeStart), 
                              (encodeEnd - encodeStart) / 1000.0f);
            } else {
                Serial.println("--> ERROR: Gagal melakukan encode!");
            }
        }
    }

    // 4. Output Hasil
    Serial.println("\n[3/3] Hasil Array Kompresi Y (M = 32):");
    Serial.print("[");
    for (uint8_t i = 0; i < CS_M; i++) {
        Serial.printf("%.4f", compressedData[i]);
        if (i < CS_M - 1) Serial.print(", ");
    }
    Serial.println("]");
    
    Serial.println("\n=== TES SELESAI ===");
    Serial.println("Untuk mencoba kompresi data riil, Anda dapat menggabungkan CSEncoder");
    Serial.println("ke dalam file main.cpp, dengan mem-push nilai IMU/PPG ke dalamnya.");
}

void loop() {
    // Kosong, test hanya berjalan 1x di setup
    delay(1000);
}

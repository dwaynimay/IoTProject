// File: firmware/test_sketches/test_imu.cpp
// Deskripsi: Test sketch MPU6050 (IMU) — output CSV untuk analisis akurasi.

#include <Arduino.h>
#include <Wire.h>
#include "Sensor_MPU.h"

SensorMPU imu;

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println("\n--- TEST IMU MPU6050 ---");

    if (!imu.begin()) {
        Serial.println("GAGAL inisialisasi sensor IMU!");
        while (true) { delay(1000); }
    }

    // imu.begin() sudah otomatis me-load kalibrasi dari memori NVS.
    // Jika Anda ingin mengkalibrasi ulang, hapus tanda komentar (/* ... */) di bawah:

    // Serial.println("Sensor siap. Kalibrasi dimulai...");
    // Serial.println(">> Letakkan sensor DATAR & DIAM selama ~2.5 detik <<");
    // delay(500);
    // imu.calibrate(500);
    // Serial.println("Kalibrasi selesai dan disimpan ke NVS!\n");

    Serial.println("Menggunakan data kalibrasi NVS tersimpan.");

    // Header CSV
    // Kolom: AccelX(m/s²), AccelY(m/s²), AccelZ(m/s²),
    //        GyroX(°/s),   GyroY(°/s),   GyroZ(°/s),
    //        AccelMag(g)
    Serial.println("AX,AY,AZ,GX,GY,GZ,AccelMag_g");
}

void loop() {
    static uint32_t lastPrintMs = 0;
    if (millis() - lastPrintMs >= 20) {   // 50 Hz (sama seperti test_ppg)
        lastPrintMs = millis();

        ImuMeasurement data;
        if (imu.read(data)) {
            // Hitung magnitudo akselerasi dalam satuan g
            // Saat diam & datar, seharusnya ~1.00 g (gravitasi saja)
            const float mag_g = sqrtf(data.accelX * data.accelX
                                    + data.accelY * data.accelY
                                    + data.accelZ * data.accelZ) / 9.80665f;

            Serial.print(data.accelX, 3); Serial.print(",");
            Serial.print(data.accelY, 3); Serial.print(",");
            Serial.print(data.accelZ, 3); Serial.print(",");
            Serial.print(data.gyroX, 2);  Serial.print(",");
            Serial.print(data.gyroY, 2);  Serial.print(",");
            Serial.print(data.gyroZ, 2);  Serial.print(",");
            Serial.println(mag_g, 4);
        }
    }
}

// ============================================================================
// CARA PAKAI:
//   1. Build & upload:  pio run -e test_imu -t upload
//   2. Buka Serial Monitor (115200 baud)
//   3. Tunggu kalibrasi selesai (~2.5 detik, sensor harus diam & datar)
//   4. Data CSV akan mengalir di layar
//
// CARA REKAM CSV:
//   python scripts/record_ppg_csv.py [COM_PORT]
//   (script yang sama bisa dipakai — ia merekam semua baris berkoma)
//
// CARA CEK AKURASI:
//   - Letakkan sensor DATAR & DIAM:
//       AX ≈ 0.00,  AY ≈ 0.00,  AZ ≈ 9.81  (gravitasi di sumbu Z)
//       GX ≈ 0.00,  GY ≈ 0.00,  GZ ≈ 0.00
//       AccelMag_g ≈ 1.0000
//
//   - Miringkan 90° ke kiri (sumbu Y jadi vertikal):
//       AX ≈ 0.00,  AY ≈ 9.81,  AZ ≈ 0.00
//
//   - Balikkan (terbalik):
//       AX ≈ 0.00,  AY ≈ 0.00,  AZ ≈ -9.81
//
//   - AccelMag_g harus SELALU ≈ 1.00 dalam keadaan diam
//     (tidak peduli orientasi). Jika menyimpang > ±0.05g → kalibrasi buruk
//     atau sensor KW berkualitas rendah.
//
//   - Gyro saat diam harus < ±1.0 °/s. Jika > ±2.0 → sensor noisy/KW.
// ============================================================================

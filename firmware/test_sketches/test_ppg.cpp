// File: firmware/test_sketches/test_ppg.cpp
// Deskripsi: Test sketch MAX30102 (PPG) di PERGELANGAN — arsitektur modular.

#include <Arduino.h>
#include <Wire.h>
#include "Sensor_PPG.h"

SensorPPG sensor;

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println("\n--- TEST PPG WRIST (modular) ---");
    Wire.begin(18, 19);

    if (!sensor.begin()) {
        Serial.println("GAGAL inisialisasi sensor PPG!");
        while (true) { delay(1000); }
    }

    Serial.println("Sensor siap.");
    // Kolom: BPF, Threshold, Status(0=ok,50=motion,100=lost), BPM
    Serial.println("BPF,Threshold,Status,BPM");
}

void loop() {
    sensor.update();

    static uint32_t lastPrintMs = 0;
    if (millis() - lastPrintMs >= 20) {   // 50 Hz
        lastPrintMs = millis();

        // Status gabungan: 100=sinyal hilang, 50=motion, 0=normal
        int status = 0;
        if (sensor.isSignalLost()) status = 100;
        else if (sensor.isMotion()) status = 50;

        Serial.print(sensor.getAcIr());        // sinyal band-pass
        Serial.print(",");
        Serial.print(sensor.getThreshold());
        Serial.print(",");
        Serial.print(status);
        Serial.print(",");
        Serial.println(sensor.getBpm());
    }
}

// CARA BACA STATUS:
//   Status=0   -> normal, BPM valid
//   Status=50  -> sedang gerakan, BPM dibekukan
//   Status=100 -> SINYAL HILANG (kontak buruk/amplitudo kecil), BPM basi!
//
// Jika Status sering 100 -> sinyal terlalu lemah. Solusi:
//   - Tekan/strap sensor lebih erat ke arteri (sisi ibu jari)
//   - Naikkan LED_POWER_WRIST di Sensor_PPG.h
//   - Pastikan tidak ada celah cahaya antara sensor dan kulit 
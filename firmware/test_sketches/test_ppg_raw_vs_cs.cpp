// File: firmware/test_sketches/test_ppg_raw_vs_cs.cpp
//
// Pengujian isolasi PPG raw vs compressive sensing.
// Sketch ini:
//   1. Membaca sinyal IR mentah dari MAX30102.
//   2. Mengumpulkan 64 sampel per window.
//   3. Menghitung hasil kompresi CS (32 measurement).
//   4. Mencetak JSON ke serial agar bisa direkam script Python.

#include <Arduino.h>
#include <Wire.h>
#include "Sensor_PPG.h"
#include "CS_Sensor.h"

SensorPPG ppg;
CSEncoder encIr;

float rawIr[CS_N];

static void printJsonArray(const char* key, const float* arr, int len)
{
    Serial.print(",\"");
    Serial.print(key);
    Serial.print("\":[");
    for (int i = 0; i < len; i++)
    {
        Serial.print(arr[i], 3);
        if (i < len - 1) Serial.print(",");
    }
    Serial.print("]");
}

void setup()
{
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Wire.begin(18, 19);

    if (!ppg.begin())
    {
        Serial.println("{\"error\":\"Gagal inisialisasi sensor PPG\"}");
        while (true) { delay(1000); }
    }
}

void loop()
{
    static uint32_t lastSampleMs = 0;

    ppg.update();

    // Mengikuti test_ppg.cpp: sampling serial efektif 50 Hz (20 ms).
    if (millis() - lastSampleMs < 20) return;
    lastSampleMs = millis();

    PpgMeasurement data;
    if (!ppg.read(data)) return;

    uint8_t idx = encIr.count();
    if (idx < CS_N)
    {
        rawIr[idx] = data.irRaw;
    }

    bool ready = encIr.pushSample(data.irRaw);
    if (!ready) return;

    float yIr[CS_M];
    float meanIr;
    encIr.encode(yIr, meanIr);

    Serial.print("{\"type\":\"cs_test_ppg\"");
    printJsonArray("ir_raw", rawIr, CS_N);
    printJsonArray("ir_cs", yIr, CS_M);
    Serial.print(",\"mean_ir\":");
    Serial.print(meanIr, 3);
    Serial.print(",\"hr\":");
    Serial.print(data.heartRate);
    Serial.print(",\"spo2\":");
    Serial.print(data.spo2, 2);
    Serial.print(",\"ppg_valid\":");
    Serial.print(data.valid ? "true" : "false");
    Serial.println("}");
}

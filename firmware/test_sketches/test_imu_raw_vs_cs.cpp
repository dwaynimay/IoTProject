// File: firmware/test_sketches/test_imu_raw_vs_cs.cpp

#include <Arduino.h>
#include <Wire.h>
#include "Sensor_MPU.h"
#include "CS_Sensor.h"

SensorMPU imu;

CSEncoder encAx, encAy, encAz;
CSEncoder encGx, encGy, encGz;

// Array untuk menyimpan data raw agar bisa di-print berbarengan dengan CS
float rawAx[CS_N], rawAy[CS_N], rawAz[CS_N];
float rawGx[CS_N], rawGy[CS_N], rawGz[CS_N];

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    // Init MPU
    if (!imu.begin()) {
        Serial.println("{\"error\": \"Gagal inisialisasi sensor IMU!\"}");
        while (true) { delay(1000); }
    }
}

// Helper function to print array in JSON format
void printJsonArray(const char* key, const float* arr, int len) {
    Serial.print(",\"");
    Serial.print(key);
    Serial.print("\":[");
    for (int i = 0; i < len; i++) {
        Serial.print(arr[i], 3);
        if (i < len - 1) Serial.print(",");
    }
    Serial.print("]");
}

void loop() {
    static uint32_t lastSampleMs = 0;
    
    // Sampling setiap 10ms (100 Hz)
    if (millis() - lastSampleMs >= 10) {
        lastSampleMs = millis();
        
        ImuMeasurement data;
        if (imu.read(data)) {
            uint8_t idx = encAx.count();
            if (idx < CS_N) {
                rawAx[idx] = data.accelX;
                rawAy[idx] = data.accelY;
                rawAz[idx] = data.accelZ;
                rawGx[idx] = data.gyroX;
                rawGy[idx] = data.gyroY;
                rawGz[idx] = data.gyroZ;
            }
            
            bool ready = encAx.pushSample(data.accelX);
            encAy.pushSample(data.accelY);
            encAz.pushSample(data.accelZ);
            encGx.pushSample(data.gyroX);
            encGy.pushSample(data.gyroY);
            encGz.pushSample(data.gyroZ);
            
            if (ready) {
                float yAx[CS_M], yAy[CS_M], yAz[CS_M];
                float yGx[CS_M], yGy[CS_M], yGz[CS_M];
                float meanAx, meanAy, meanAz, meanGx, meanGy, meanGz;
                
                encAx.encode(yAx, meanAx);
                encAy.encode(yAy, meanAy);
                encAz.encode(yAz, meanAz);
                encGx.encode(yGx, meanGx);
                encGy.encode(yGy, meanGy);
                encGz.encode(yGz, meanGz);
                
                // Manual JSON building to avoid ArduinoJson memory overhead limits
                Serial.print("{\"type\":\"cs_test\"");
                
                printJsonArray("ax_raw", rawAx, CS_N);
                printJsonArray("ax_cs", yAx, CS_M);
                Serial.print(",\"mean_ax\":"); Serial.print(meanAx, 3);
                
                printJsonArray("ay_raw", rawAy, CS_N);
                printJsonArray("ay_cs", yAy, CS_M);
                Serial.print(",\"mean_ay\":"); Serial.print(meanAy, 3);
                
                printJsonArray("az_raw", rawAz, CS_N);
                printJsonArray("az_cs", yAz, CS_M);
                Serial.print(",\"mean_az\":"); Serial.print(meanAz, 3);

                printJsonArray("gx_raw", rawGx, CS_N);
                printJsonArray("gx_cs", yGx, CS_M);
                Serial.print(",\"mean_gx\":"); Serial.print(meanGx, 3);
                
                printJsonArray("gy_raw", rawGy, CS_N);
                printJsonArray("gy_cs", yGy, CS_M);
                Serial.print(",\"mean_gy\":"); Serial.print(meanGy, 3);
                
                printJsonArray("gz_raw", rawGz, CS_N);
                printJsonArray("gz_cs", yGz, CS_M);
                Serial.print(",\"mean_gz\":"); Serial.print(meanGz, 3);
                
                Serial.println("}");
            }
        }
    }
}

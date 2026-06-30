#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "Sensor_MPU.h"
#include "CS_Sensor.h"
#include "config/credentials.h"

// ID Node untuk di publish ke MQTT
#ifndef NODE_ID
#define NODE_ID 1
#endif

// Stringify dua tingkat agar nilai NODE_ID ikut di-expand (1 -> "1", bukan "NODE_ID")
#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)

SensorMPU imu;

CSEncoder encAx, encAy, encAz;
CSEncoder encGx, encGy, encGz;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

const char* topic_cs_imu = "health_monitor/node_" STRINGIFY(NODE_ID) "/cs_imu";
const char* topic_cs_ppg = "health_monitor/node_" STRINGIFY(NODE_ID) "/cs_ppg";

void setup_wifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to WiFi: ");
    Serial.println(Wifi::SSID);

    WiFi.begin(Wifi::SSID, Wifi::PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

void reconnect() {
    while (!mqttClient.connected()) {
        Serial.print("Attempting MQTT connection...");
        if (mqttClient.connect(Mqtt::CLIENT_ID, Mqtt::USER, Mqtt::PASSWORD)) {
            Serial.println("connected");
        } else {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" try again in 5 seconds");
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    setup_wifi();
    mqttClient.setServer(Mqtt::BROKER, Mqtt::PORT);
    // Tingkatkan buffer size karena JSON CS cukup besar (sekitar 1-2KB)
    mqttClient.setBufferSize(2048); 

    if (!imu.begin()) {
        Serial.println("Gagal inisialisasi sensor IMU!");
        while (true) { delay(1000); }
    }
    Serial.println("IMU Ready. Memulai perekaman direct-to-MQTT...");
}

// Helper function untuk menambahkan array ke string JSON
void appendJsonArray(String& json, const char* key, const float* arr, int len) {
    json += "\"";
    json += key;
    json += "\":[";
    for (int i = 0; i < len; i++) {
        json += String(arr[i], 3);
        if (i < len - 1) json += ",";
    }
    json += "]";
}

void loop() {
    if (!mqttClient.connected()) {
        reconnect();
    }
    mqttClient.loop();

    static uint32_t lastSampleMs = 0;
    
    // Sampling setiap 10ms (100 Hz)
    if (millis() - lastSampleMs >= 10) {
        lastSampleMs = millis();
        
        ImuMeasurement data;
        if (imu.read(data)) {
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
                
                // Construct JSON string manual to save ArduinoJson overhead
                String payload = "{";
                payload += "\"ts\":" + String(millis()) + ",";
                payload += "\"finger\":false,";
                
                appendJsonArray(payload, "ax", yAx, CS_M); payload += ",";
                payload += "\"mean_ax\":" + String(meanAx, 3) + ",";
                
                appendJsonArray(payload, "ay", yAy, CS_M); payload += ",";
                payload += "\"mean_ay\":" + String(meanAy, 3) + ",";
                
                appendJsonArray(payload, "az", yAz, CS_M); payload += ",";
                payload += "\"mean_az\":" + String(meanAz, 3) + ",";

                appendJsonArray(payload, "gx", yGx, CS_M); payload += ",";
                payload += "\"mean_gx\":" + String(meanGx, 3) + ",";
                
                appendJsonArray(payload, "gy", yGy, CS_M); payload += ",";
                payload += "\"mean_gy\":" + String(meanGy, 3) + ",";
                
                appendJsonArray(payload, "gz", yGz, CS_M); payload += ",";
                payload += "\"mean_gz\":" + String(meanGz, 3);
                
                payload += "}";

                // Publish IMU
                bool pubSuccess = mqttClient.publish(topic_cs_imu, payload.c_str(), false); // no retain
                
                // Construct Dummy PPG Payload agar node_state.py di server tidak discard data ini
                // Karena server butuh pasangan IMU dan PPG untuk bisa masuk ke SQLite.
                String dummyPpg = "{";
                dummyPpg += "\"ts\":" + String(millis()) + ",";
                dummyPpg += "\"finger\":false,";
                dummyPpg += "\"mean_ir\":0.0,";
                dummyPpg += "\"hr\":0,";
                
                // array dummy untuk ir (sepanjang CS_M)
                dummyPpg += "\"ir\":[";
                for (int i = 0; i < CS_M; i++) {
                    dummyPpg += "0.0";
                    if (i < CS_M - 1) dummyPpg += ",";
                }
                dummyPpg += "]}";
                
                // Publish Dummy PPG
                bool ppgSuccess = mqttClient.publish(topic_cs_ppg, dummyPpg.c_str(), false);
                
                if (pubSuccess && ppgSuccess) {
                    Serial.println("Published 1 CS Window (IMU + Dummy PPG)");
                } else {
                    Serial.println("Gagal publish MQTT!");
                }
            }
        }
    }
}

// =============================================================================
// task_cs_sender.cpp — Task CS encode & kirim (Core 0)
//
// PERUBAHAN dari versi sebelumnya:
//   - CSPhiMatrix singleton otomatis di-init saat CSEncoder pertama dibuat
//   - Tambah CSPhiMatrix::printInfo() di log awal untuk verifikasi
//   - Semua logika encode & kirim TIDAK berubah
//
// Kirim 7 paket CS1AxisPacket per window (1 per sinyal):
//   PKT_CS_AX, PKT_CS_AY, PKT_CS_AZ,
//   PKT_CS_GX, PKT_CS_GY, PKT_CS_GZ,
//   PKT_CS_IR
//
// Semua 7 paket dalam 1 window pakai timestamp yang SAMA (diambil sekali
// sebelum kirim) → server tidak warning "timestamp spread Xms".
// =============================================================================

#include <Arduino.h>
#include <esp_now.h>
#include "Config.h"
#include "DataModels.h"
#include "DataModels_CS.h"
#include "CS_Sensor.h"
#include "Watchdog.h"

extern portMUX_TYPE g_stateMux;
extern ImuSample    g_latestImu;
extern PpgSample    g_latestPpg;

// 7 encoder — static agar tidak di stack.
// Sekarang masing-masing hanya 261 byte (pointer ke singleton Φ + buffer).
// Total: 7 × 261 = 1.827 byte  (vs 7 × 8.448 = 59.136 byte sebelumnya)
static CSEncoder g_enc_ax, g_enc_ay, g_enc_az;
static CSEncoder g_enc_gx, g_enc_gy, g_enc_gz;
static CSEncoder g_enc_ir;

// ---------------------------------------------------------------------------
// Helper: kirim 1 axis sebagai CS1AxisPacket
// ts_window diambil SEKALI sebelum loop kirim → timestamp konsisten
// ---------------------------------------------------------------------------
static void sendAxis(uint8_t pktType, uint8_t nodeId,
                     const float y[CS_M], bool fingerOn,
                     uint32_t ts_window)
{
    CS1AxisPacket pkt{};
    pkt.header.type      = static_cast<PacketType>(pktType);
    pkt.header.node_id   = nodeId;
    pkt.header.timestamp = ts_window;
    memcpy(pkt.y, y, CS_M * sizeof(float));
    pkt.edge = {fingerOn, 0};

    esp_now_send(MacAddr::GATEWAY,
                 reinterpret_cast<uint8_t *>(&pkt), sizeof(CS1AxisPacket));
    vTaskDelay(pdMS_TO_TICKS(1)); // 1ms jeda antar paket
}

// ---------------------------------------------------------------------------
// taskCSSender — Core 0
// ---------------------------------------------------------------------------
void taskCSSender(void *param)
{
    g_watchdog.registerTask();

    // Output encoder — di stack task
    // Stack ESPNOW_TX harus >= 12288 (diset di config/tuning.h)
    float y_ax[CS_M], y_ay[CS_M], y_az[CS_M];
    float y_gx[CS_M], y_gy[CS_M], y_gz[CS_M];
    float y_ir[CS_M];

    uint32_t windowCount = 0;
    uint32_t lastLog     = 0;

    // Log info singleton Φ — untuk verifikasi seed dan ukuran RAM
    CSPhiMatrix::printInfo();
    CSPhiMatrix::printSyncDebug(); // bandingkan dengan: python -m server.verify_phi
    Serial.printf("[CS] 7 encoder aktif | N=%d M=%d (%.0f%%) | RAM encoder: ~%d byte\n",
                  CS_N, CS_M, 100.0f * CS_M / CS_N,
                  (int)(7 * (CS_N * sizeof(float) + sizeof(void *))));
    Serial.printf("[CS] Window duration: %d ms\n", CS_N * Timing::IMU_SAMPLE_MS);

    for (;;)
    {
        g_watchdog.feed();

        // Snapshot shared state — atomic
        ImuSample imu{};
        PpgSample ppg{};
        taskENTER_CRITICAL(&g_stateMux);
        imu = g_latestImu;
        ppg = g_latestPpg;
        taskEXIT_CRITICAL(&g_stateMux);

        // Push ke semua encoder
        bool ax_rdy = g_enc_ax.pushSample(imu.accel_x);
        bool ay_rdy = g_enc_ay.pushSample(imu.accel_y);
        bool az_rdy = g_enc_az.pushSample(imu.accel_z);
        bool gx_rdy = g_enc_gx.pushSample(imu.gyro_x);
        bool gy_rdy = g_enc_gy.pushSample(imu.gyro_y);
        bool gz_rdy = g_enc_gz.pushSample(imu.gyro_z);
        bool ir_rdy = g_enc_ir.pushSample(static_cast<float>(ppg.ir_raw));

        // Encode & kirim saat semua window penuh
        if (ax_rdy && ay_rdy && az_rdy &&
            gx_rdy && gy_rdy && gz_rdy && ir_rdy)
        {
            g_enc_ax.encode(y_ax);
            g_enc_ay.encode(y_ay);
            g_enc_az.encode(y_az);
            g_enc_gx.encode(y_gx);
            g_enc_gy.encode(y_gy);
            g_enc_gz.encode(y_gz);
            g_enc_ir.encode(y_ir);

            bool finger = (ppg.ir_raw >= EdgeConfig::IR_FINGER_THRESHOLD);

            // Ambil timestamp SEKALI untuk semua 7 paket → spread ≈ 0ms
            uint32_t ts_now = millis();

            // Kirim 6 IMU axis
            sendAxis(PKT_CS_AX, NODE_ID, y_ax, finger, ts_now);
            sendAxis(PKT_CS_AY, NODE_ID, y_ay, finger, ts_now);
            sendAxis(PKT_CS_AZ, NODE_ID, y_az, finger, ts_now);
            sendAxis(PKT_CS_GX, NODE_ID, y_gx, finger, ts_now);
            sendAxis(PKT_CS_GY, NODE_ID, y_gy, finger, ts_now);
            sendAxis(PKT_CS_GZ, NODE_ID, y_gz, finger, ts_now);

            // PPG dengan HR metadata — timestamp sama
            CSPpgPacket ppgPkt{};
            ppgPkt.header     = {static_cast<PacketType>(PKT_CS_IR), NODE_ID, ts_now};
            memcpy(ppgPkt.y_ir, y_ir, CS_M * sizeof(float));
            ppgPkt.heart_rate = ppg.heart_rate;
            ppgPkt.ppg_valid  = ppg.valid;
            ppgPkt.edge       = {finger, 0};
            esp_now_send(MacAddr::GATEWAY,
                         reinterpret_cast<uint8_t *>(&ppgPkt), sizeof(CSPpgPacket));

            windowCount++;

            // Log setiap 5 window (~3 detik)
            if (windowCount % 5 == 0)
            {
                Serial.printf("[CS TX] Window #%lu | finger=%s | HR=%d | ts=%lu\n",
                              windowCount, finger ? "Y" : "N",
                              ppg.heart_rate, ts_now);
            }
        }

        // Debug log setiap 10 detik
        if (millis() - lastLog >= 10000)
        {
            Serial.printf("[CS DBG] buf=%d/%d | sent=%lu windows | heap=%luKB\n",
                          g_enc_ax.count(), CS_N,
                          windowCount,
                          esp_get_free_heap_size() / 1024);
            lastLog = millis();
        }

        // Stack check setiap 500 iterasi
        static uint32_t iter = 0;
        if (++iter % 500 == 0)
            g_watchdog.checkTaskStack("CS_TX");

        vTaskDelay(pdMS_TO_TICKS(Timing::IMU_SAMPLE_MS)); // 100 Hz
    }
}
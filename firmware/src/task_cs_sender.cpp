// File: src/task_cs_sender.cpp

// =============================================================================
// task_cs_sender.cpp — Task CS Encode & Kirim (Sensor Node, Core 0)
// =============================================================================
//
// Tanggung jawab:
//   1. Baca snapshot IMU + PPG dari shared state (g_latestImu, g_latestPpg)
//   2. Push sample ke 7 CSEncoder (ax, ay, az, gx, gy, gz, ir)
//   3. Saat window penuh (CS_N sampel), encode → y = Φ · x
//   4. Kirim 7 paket CS via g_mesh.sendCsAxis() dan g_mesh.sendCsPpg()
//
// Timestamp diambil SEKALI sebelum loop kirim → semua 7 paket punya
// timestamp yang sama → server tidak warning "timestamp spread Xms".
//
// Stack requirement: >= 12288 bytes (diset di config/tuning.h)
// Alasan: 7 × float[CS_M] lokal = 7 × 128 = 896 bytes di stack,
//         plus overhead snprintf dan encode loop.
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "CS_Sensor.h"
#include "EspNowMesh.h"
#include "MeshPackets.h"
#include "Watchdog.h"

// Shared state dari main.cpp (diupdate oleh taskReadIMU dan taskReadPPG)
extern portMUX_TYPE g_stateMux;
extern ImuSample    g_latestImu;
extern PpgSample    g_latestPpg;

// Instance mesh dari main.cpp
extern EspNowMesh g_mesh;

static constexpr char TAG[] = "CS_TX";

// =============================================================================
// Encoder instances — static agar tidak di stack task
//
// Static di sini artinya lifetime = program lifetime, bukan per-call.
// Masing-masing encoder hanya butuh ~261 byte (pointer Φ + buffer CS_N float)
// karena Φ di-share via CSPhiMatrix singleton.
//
// Total RAM: 7 × 261 = 1.827 byte (vs 7 × 8.448 = 59.136 byte versi lama)
// =============================================================================
static CSEncoder g_encAx, g_encAy, g_encAz;
static CSEncoder g_encGx, g_encGy, g_encGz;
static CSEncoder g_encIr;


// =============================================================================
// taskCSSender — Entry Point Task
// =============================================================================
void taskCSSender(void* param)
{
    g_watchdog.registerTask();

    // Output buffer encoder — di stack task, bukan global
    // Stack harus >= 12288 bytes untuk menampung ini + overhead
    float yAx[CS_M], yAy[CS_M], yAz[CS_M];
    float yGx[CS_M], yGy[CS_M], yGz[CS_M];
    float yIr[CS_M];

    uint32_t windowCount = 0;

    // Log info Φ saat startup untuk verifikasi sinkronisasi dengan server
    CSPhiMatrix::printInfo();
    CSPhiMatrix::printSyncDebug();

    LOG_INFO(TAG, "7 encoder aktif | N=%d M=%d (%.0f%%) | RAM encoder: ~%d bytes",
             CS_N, CS_M, 100.0f * CS_M / CS_N,
             (int)(7 * (CS_N * sizeof(float) + sizeof(void*))));
    LOG_INFO(TAG, "Window duration: %d ms | stack: %lu bytes",
             CS_N * Timing::IMU_SAMPLE_MS, StackSize::ESPNOW_TX);

    for (;;)
    {
        g_watchdog.feed();

        // ── Snapshot shared state (atomic) ────────────────────────────────────
        ImuSample imu{};
        PpgSample ppg{};
        taskENTER_CRITICAL(&g_stateMux);
        imu = g_latestImu;
        ppg = g_latestPpg;
        taskEXIT_CRITICAL(&g_stateMux);

        // ── Push sample ke semua encoder ──────────────────────────────────────
        const bool axRdy = g_encAx.pushSample(imu.accelX);
        const bool ayRdy = g_encAy.pushSample(imu.accelY);
        const bool azRdy = g_encAz.pushSample(imu.accelZ);
        const bool gxRdy = g_encGx.pushSample(imu.gyroX);
        const bool gyRdy = g_encGy.pushSample(imu.gyroY);
        const bool gzRdy = g_encGz.pushSample(imu.gyroZ);
        const bool irRdy = g_encIr.pushSample(static_cast<float>(ppg.irRaw));

        // ── Encode & kirim saat semua window penuh ────────────────────────────
        // Semua encoder harus penuh bersamaan karena mereka diisi dengan
        // sample dari snapshot yang sama. Jika salah satu tidak siap,
        // berarti ada bug di logika reset encoder.
        if (axRdy && ayRdy && azRdy && gxRdy && gyRdy && gzRdy && irRdy)
        {
            g_encAx.encode(yAx);
            g_encAy.encode(yAy);
            g_encAz.encode(yAz);
            g_encGx.encode(yGx);
            g_encGy.encode(yGy);
            g_encGz.encode(yGz);
            g_encIr.encode(yIr);

            const bool     finger  = (ppg.irRaw >= EdgeConfig::IR_FINGER_THRESHOLD);
            const uint32_t tsNow   = millis(); // satu timestamp untuk semua 7 paket

            // ── Kirim 6 IMU axis dan 1 PPG IR dengan NACK checking ────────────
            uint8_t nack = 0;
            
            if (!g_mesh.sendCsAxis(PKT_CS_AX, NODE_ID, yAx, finger, tsNow)) nack++;
            if (!g_mesh.sendCsAxis(PKT_CS_AY, NODE_ID, yAy, finger, tsNow)) nack++;
            if (!g_mesh.sendCsAxis(PKT_CS_AZ, NODE_ID, yAz, finger, tsNow)) nack++;
            if (!g_mesh.sendCsAxis(PKT_CS_GX, NODE_ID, yGx, finger, tsNow)) nack++;
            if (!g_mesh.sendCsAxis(PKT_CS_GY, NODE_ID, yGy, finger, tsNow)) nack++;
            if (!g_mesh.sendCsAxis(PKT_CS_GZ, NODE_ID, yGz, finger, tsNow)) nack++;
            
            if (!g_mesh.sendCsPpg(NODE_ID, yIr, ppg.heartRate,
                                   ppg.valid, finger, tsNow)) nack++;

            windowCount++;

            if (nack > 0)
            {
                LOG_WARN(TAG, "Window #%lu — %d/7 paket gagal antre TX!", windowCount, nack);
            }
            else
            {
                LOG_EVERY_N(5, LOG_DEBUG, TAG, "Window #%lu — 7/7 TX OK", windowCount);
            }

            // Log setiap 5 window (~3.2 detik pada 100Hz IMU, N=64)
            if (windowCount % 5 == 0)
            {
                LOG_INFO(TAG, "Window #%lu | finger=%s | HR=%d | ts=%lu ms",
                         windowCount,
                         finger ? "Y" : "N",
                         ppg.heartRate,
                         tsNow);
            }
        }

        // Log buffer status setiap 10 detik
        LOG_EVERY_N(1000, LOG_DEBUG, TAG,
                    "buf=%d/%d | windows=%lu | heap=%lu KB",
                    g_encAx.count(), CS_N,
                    windowCount,
                    esp_get_free_heap_size() / 1024);

        // Stack check setiap 500 iterasi
        LOG_EVERY_N(500, LOG_DEBUG, TAG,
                    "Stack watermark: %u bytes",
                    uxTaskGetStackHighWaterMark(NULL));

        vTaskDelay(pdMS_TO_TICKS(Timing::IMU_SAMPLE_MS)); // 100 Hz
    }
}
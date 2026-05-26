// File: firmware/src/task_cs_sender.cpp

// =============================================================================
// task_cs_sender.cpp — Task CS Encode & Kirim dengan Dynamic Routing
// =============================================================================
//
// PERUBAHAN v3.1 (F2 — Sensor Sanity Check):
//
//   Namespace SanityLimit:
//     ACCEL_MAX_MS2  : batas |ax|,|ay|,|az| dalam m/s²
//                      ±25 m/s² ≈ 2.5g — sensor fault biasanya > 100 m/s²
//     GYRO_MAX_DEGS  : batas |gx|,|gy|,|gz| dalam °/s
//                      ±300 °/s — gerakan manusia normal < 200 °/s
//     IR_ZERO_IF_NO_FINGER : jika true, set irSample=0.0f saat finger=false
//                            agar encoder tidak encode noise DC MAX30102
//
//   _imuInRange(imu) → bool:
//     Return false jika salah satu axis di luar batas.
//     Dicatat ke g_droppedWindows dan di-log setiap LOG_DROP_EVERY window.
//
//   Counter g_droppedWindows:
//     Ekspos via extern agar taskMonitor bisa print ke watchdog / serial.
//
// ALUR BARU di taskCSSender:
//   1. Snapshot IMU + PPG
//   2. _imuInRange() → jika gagal, reset semua encoder + skip window
//   3. IR sample → 0 jika finger=false dan IR_ZERO_IF_NO_FINGER=true
//   4. pushSample → encode → route → kirim (sama seperti sebelumnya)
//
// =============================================================================

#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include "Config.h"

#if NODE_ROLE == ROLE_SENSOR
#include "CS_Sensor.h"
#include "EspNowMesh.h"
#include "MeshPackets.h"
#include "Watchdog.h"
#include "DynamicRouter.h"

extern portMUX_TYPE g_stateMux;
extern ImuSample    g_latestImu;
extern PpgSample    g_latestPpg;
extern EspNowMesh   g_mesh;

static constexpr char TAG[] = "CS_TX";


// =============================================================================
// Namespace SanityLimit — batas fisis sensor
// =============================================================================
namespace SanityLimit
{
    // Batas accelerometer: ±25 m/s² ≈ ±2.5g
    // Sensor MPU6050 dikonfigurasi FS_SEL=0 (±2g = ±19.6 m/s²).
    // Diberi margin sedikit di atas FS range untuk tangkap saturasi.
    // Fault biasanya muncul sebagai nilai ekstrem > 100 m/s².
    constexpr float ACCEL_MAX_MS2 = 25.0f;

    // Batas gyroscope: ±300 °/s
    // MPU6050 FS_SEL=0: ±250 °/s. Margin kecil di atas FS range.
    // Gerakan tangan manusia normal: < 200 °/s.
    constexpr float GYRO_MAX_DEGS = 300.0f;

    // Jika true, set irSample = 0.0f saat finger = false.
    // Mencegah encoder menyimpan noise DC dari MAX30102 yang tidak ada jari.
    // Set false jika ingin tetap encode IR meskipun tidak ada jari
    // (misalnya untuk debugging baseline PPG).
    constexpr bool IR_ZERO_IF_NO_FINGER = true;

    // Log setiap N window yang di-drop (mencegah banjir log)
    constexpr uint32_t LOG_DROP_EVERY = 10;
}


// =============================================================================
// Counter global — bisa di-extern oleh taskMonitor untuk diagnostik
// =============================================================================
uint32_t g_droppedWindows = 0; // window yang dibuang karena sanity check gagal


// =============================================================================
// Encoder instances
// =============================================================================
static CSEncoder g_encAx, g_encAy, g_encAz;
static CSEncoder g_encGx, g_encGy, g_encGz;
static CSEncoder g_encIr;


// =============================================================================
// DynamicRouter — satu per sensor node
// =============================================================================
static DynamicRouter g_router(NODE_ID);

// Ekspos ke EspNowMesh agar _onDataRecv bisa update RSSI
DynamicRouter* g_routerPtr = &g_router;


// =============================================================================
// _imuInRange() — Cek apakah semua axis IMU dalam batas fisis
//
// Return true  → data valid, lanjut encode
// Return false → data anomali, drop window ini
//
// Mengapa cek sebelum encode (bukan setelah)?
//   CS encoder adalah operasi linear: y = Φ · x.
//   Jika x berisi outlier besar, seluruh vektor y corrupt.
//   Lebih mudah dan murah drop di sini daripada filter setelah encode.
// =============================================================================
static bool _imuInRange(const ImuSample& imu)
{
    // Cek accelerometer
    if (fabsf(imu.accelX) > SanityLimit::ACCEL_MAX_MS2 ||
        fabsf(imu.accelY) > SanityLimit::ACCEL_MAX_MS2 ||
        fabsf(imu.accelZ) > SanityLimit::ACCEL_MAX_MS2)
    {
        return false;
    }

    // Cek gyroscope
    if (fabsf(imu.gyroX) > SanityLimit::GYRO_MAX_DEGS ||
        fabsf(imu.gyroY) > SanityLimit::GYRO_MAX_DEGS ||
        fabsf(imu.gyroZ) > SanityLimit::GYRO_MAX_DEGS)
    {
        return false;
    }

    return true;
}


// =============================================================================
// _resetAllEncoders() — Reset semua encoder saat window di-drop
//
// Penting: jika buffer encoder sudah terisi sebagian lalu window di-drop,
// sampel lama harus dibuang agar tidak tercampur dengan window berikutnya.
// =============================================================================
static void _resetAllEncoders()
{
    g_encAx.reset(); g_encAy.reset(); g_encAz.reset();
    g_encGx.reset(); g_encGy.reset(); g_encGz.reset();
    g_encIr.reset();
}


// =============================================================================
// Helper: pilih MAC tujuan berdasarkan RouteDecision (tidak berubah)
// =============================================================================
static const uint8_t* _selectDstMac(const RouteDecision& dec)
{
    if (dec.isDirect)
        return MacAddr::GATEWAY;

    #if NODE_ID == 1
        return MacAddr::NODE_B;
    #else
        return MacAddr::NODE_A;
    #endif
}


// =============================================================================
// taskRssiExchange — tidak berubah dari v3.0
// =============================================================================
void taskRssiExchange(void* param)
{
    static constexpr char RTAG[] = "RSSI_EX";

    // v4.0: Tunggu gateway ditemukan sebelum mulai exchange
    LOG_INFO(RTAG, "Menunggu koneksi gateway (background discovery)...");
    while (!g_mesh.isChannelConfirmed())
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    LOG_INFO(RTAG, "Gateway terdeteksi — mulai RSSI exchange");

    LOG_INFO(RTAG, "taskRssiExchange dimulai | interval=%lu ms",
             (unsigned long)RoutingCfg::RSSI_EXCHANGE_MS);

    vTaskDelay(pdMS_TO_TICKS(RoutingCfg::DISCOVERY_PHASE_MS));

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(RoutingCfg::RSSI_EXCHANGE_MS));

        const int8_t myRssi = g_mesh.getLastBeaconRssi();

        // ── SEBELUM: langsung skip jika -127
        // ── SESUDAH: update router dulu, baru putuskan kirim exchange
        if (myRssi != RoutingCfg::RSSI_UNKNOWN)
        {
            g_router.updateSelfRssi(myRssi);
        }
        else
        {
            LOG_EVERY_N(5, LOG_WARN, RTAG,
                        "Belum terima beacon dari gateway — RSSI belum valid");
            // Tidak continue — tetap kirim report ke neighbor
            // agar neighbor tahu kita ada, walau RSSI belum valid
        }

        // Kirim RSSI report ke neighbor terlepas dari validitas RSSI self
        // Gunakan nilai terbaik yang ada (bisa RSSI_UNKNOWN)
        const bool ok = g_mesh.sendRssiReport(NODE_ID, myRssi);

        LOG_DEBUG(RTAG, "RSSI exchange | self=%d dBm | ok=%s",
                  myRssi, ok ? "Y" : "N");

        if ((millis() / RoutingCfg::RSSI_EXCHANGE_MS) % 5 == 0)
            g_router.printStatus();
    }
}


// =============================================================================
// taskCSSender — Entry Point Task
// =============================================================================
void taskCSSender(void* param)
{
    g_watchdog.registerTask();

    // v4.0: Tunggu gateway ditemukan sebelum mulai encode & kirim
    // Task sensor (IMU, PPG) sudah jalan — hanya TX yang menunggu.
    LOG_INFO(TAG, "Menunggu koneksi gateway (background discovery)...");
    while (!g_mesh.isChannelConfirmed())
    {
        g_watchdog.feed();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    LOG_INFO(TAG, "Gateway terdeteksi — mulai encode & kirim data");

    float yAx[CS_M], yAy[CS_M], yAz[CS_M];
    float yGx[CS_M], yGy[CS_M], yGz[CS_M];
    float yIr[CS_M];

    uint32_t windowCount  = 0;
    uint32_t directCount  = 0;
    uint32_t relayedCount = 0;

    CSPhiMatrix::printInfo();
    CSPhiMatrix::printSyncDebug();

    LOG_INFO(TAG, "7 encoder aktif | N=%d M=%d | Sanity check AKTIF",
             CS_N, CS_M);
    LOG_INFO(TAG, "Limit: accel=±%.0f m/s² gyro=±%.0f °/s | IR_zero_no_finger=%s",
             SanityLimit::ACCEL_MAX_MS2,
             SanityLimit::GYRO_MAX_DEGS,
             SanityLimit::IR_ZERO_IF_NO_FINGER ? "ON" : "OFF");

    for (;;)
    {
        g_watchdog.feed();

        // ── Snapshot shared state ─────────────────────────────────────────────
        ImuSample imu{};
        PpgSample ppg{};
        taskENTER_CRITICAL(&g_stateMux);
        imu = g_latestImu;
        ppg = g_latestPpg;
        taskEXIT_CRITICAL(&g_stateMux);

        // ── Deferred channel sync (dari _promiscuousRxCb) ────────────────────────
        // Proses update peer dan channel yang ditunda.
        g_mesh.processPendingChannelSync();

        // ── F2: Sanity check IMU sebelum push ke encoder ──────────────────────
        // Drop window jika ada axis yang di luar batas fisis.
        // Reset semua encoder agar sampel lama tidak tercampur ke window baru.
        if (!_imuInRange(imu))
        {
            g_droppedWindows++;
            _resetAllEncoders();

            if (g_droppedWindows % SanityLimit::LOG_DROP_EVERY == 0)
            {
                LOG_WARN(TAG,
                         "DROPPED: IMU out of range "
                         "| ax=%.1f ay=%.1f az=%.1f m/s² "
                         "| gx=%.1f gy=%.1f gz=%.1f °/s "
                         "| total_dropped=%lu",
                         imu.accelX, imu.accelY, imu.accelZ,
                         imu.gyroX,  imu.gyroY,  imu.gyroZ,
                         g_droppedWindows);
            }

            vTaskDelay(pdMS_TO_TICKS(Timing::IMU_SAMPLE_MS));
            continue;
        }

        // ── F2: IR sample — nolkan jika tidak ada jari ────────────────────────
        // Mencegah encoder menyimpan noise DC MAX30102 sebagai sinyal valid.
        // Server tetap menerima paket (finger=false sudah ada di payload),
        // tapi measurement vector IR akan berisi nol sehingga rekonstruksi
        // menghasilkan sinyal nol — lebih bersih daripada noise.
        const float irSample = (SanityLimit::IR_ZERO_IF_NO_FINGER &&
                                !(ppg.irRaw >= EdgeConfig::IR_FINGER_THRESHOLD))
                               ? 0.0f
                               : static_cast<float>(ppg.irRaw);

        // ── Push sample ke semua encoder ──────────────────────────────────────
        const bool axRdy = g_encAx.pushSample(imu.accelX);
        const bool ayRdy = g_encAy.pushSample(imu.accelY);
        const bool azRdy = g_encAz.pushSample(imu.accelZ);
        const bool gxRdy = g_encGx.pushSample(imu.gyroX);
        const bool gyRdy = g_encGy.pushSample(imu.gyroY);
        const bool gzRdy = g_encGz.pushSample(imu.gyroZ);
        const bool irRdy = g_encIr.pushSample(irSample);

        if (!(axRdy && ayRdy && azRdy && gxRdy && gyRdy && gzRdy && irRdy))
        {
            vTaskDelay(pdMS_TO_TICKS(Timing::IMU_SAMPLE_MS));
            continue;
        }

        // ── Encode semua sinyal ───────────────────────────────────────────────
        g_encAx.encode(yAx);
        g_encAy.encode(yAy);
        g_encAz.encode(yAz);
        g_encGx.encode(yGx);
        g_encGy.encode(yGy);
        g_encGz.encode(yGz);
        g_encIr.encode(yIr);

        const bool     finger = (ppg.irRaw >= EdgeConfig::IR_FINGER_THRESHOLD);
        const uint32_t tsNow  = millis();

        // ── Dynamic Routing Decision ──────────────────────────────────────────
        const RouteDecision dec = g_router.decide();
        const uint8_t* dstMac  = _selectDstMac(dec);

        if (dec.isDirect) directCount++;
        else              relayedCount++;

        // ── Kirim 6 IMU axis + PPG ke tujuan yang dipilih ────────────────────
        uint8_t nack = 0;

        if (!g_mesh.sendCsAxis(PKT_CS_AX, NODE_ID, yAx, finger, tsNow, dstMac)) nack++;
        if (!g_mesh.sendCsAxis(PKT_CS_AY, NODE_ID, yAy, finger, tsNow, dstMac)) nack++;
        if (!g_mesh.sendCsAxis(PKT_CS_AZ, NODE_ID, yAz, finger, tsNow, dstMac)) nack++;
        if (!g_mesh.sendCsAxis(PKT_CS_GX, NODE_ID, yGx, finger, tsNow, dstMac)) nack++;
        if (!g_mesh.sendCsAxis(PKT_CS_GY, NODE_ID, yGy, finger, tsNow, dstMac)) nack++;
        if (!g_mesh.sendCsAxis(PKT_CS_GZ, NODE_ID, yGz, finger, tsNow, dstMac)) nack++;
        if (!g_mesh.sendCsPpg(NODE_ID, yIr, ppg.heartRate,
                               ppg.valid, ppg.spo2,
                               finger, tsNow, dstMac))                           nack++;

        windowCount++;

        // ── Log periodik ──────────────────────────────────────────────────────
        if (windowCount % 5 == 0)
        {
            const float relayPct = windowCount > 0
                                   ? 100.0f * relayedCount / windowCount
                                   : 0.0f;
            const float dropPct  = (windowCount + g_droppedWindows) > 0
                                   ? 100.0f * g_droppedWindows
                                     / (windowCount + g_droppedWindows)
                                   : 0.0f;

            LOG_INFO(TAG,
                     "Win #%lu [%s] | self=%d dBm neighbor=%d dBm | "
                     "relay=%.0f%% | dropped=%lu(%.1f%%) | nack=%d | "
                     "HR=%d SpO2=%.1f%%",
                     windowCount,
                     dec.isDirect ? "DIRECT" : "RELAY",
                     dec.rssiSelf,
                     dec.rssiNeighbor,
                     relayPct,
                     g_droppedWindows,
                     dropPct,
                     nack,
                     ppg.heartRate,
                     ppg.spo2 > 0 ? ppg.spo2 : 0.0f);
        }

        if (nack > 0)
        {
            LOG_WARN(TAG, "Window #%lu — %d/7 paket gagal TX ke %s!",
                     windowCount,
                     nack,
                     dec.isDirect ? "GATEWAY" : "RELAY");
        }

        LOG_EVERY_N(500, LOG_DEBUG, TAG,
                    "Stack: %u bytes | heap: %lu KB",
                    uxTaskGetStackHighWaterMark(NULL),
                    esp_get_free_heap_size() / 1024);

        vTaskDelay(pdMS_TO_TICKS(Timing::IMU_SAMPLE_MS));
    }
}

#endif // NODE_ROLE == ROLE_SENSOR

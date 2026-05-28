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
#include <esp_random.h>
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
// taskRssiExchange — v3.1: tambah retry & INFO log
// =============================================================================
void taskRssiExchange(void* param)
{
    static constexpr char RTAG[] = "RSSI_EX";

    // Tunggu gateway ditemukan sebelum mulai exchange
    LOG_INFO(RTAG, "Menunggu koneksi gateway...");
    while (!g_mesh.isChannelConfirmed())
        vTaskDelay(pdMS_TO_TICKS(1000));
    LOG_INFO(RTAG, "Gateway terdeteksi — mulai RSSI exchange | interval=%lu ms",
             (unsigned long)RoutingCfg::RSSI_EXCHANGE_MS);

    vTaskDelay(pdMS_TO_TICKS(RoutingCfg::DISCOVERY_PHASE_MS));

    uint32_t txCount   = 0;
    uint32_t failCount = 0;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(RoutingCfg::RSSI_EXCHANGE_MS));

        const int8_t myRssi = g_mesh.getLastBeaconRssi();

        if (myRssi != RoutingCfg::RSSI_UNKNOWN)
            g_router.updateSelfRssi(myRssi);
        else
            LOG_WARN(RTAG, "Belum terima beacon dari gateway — RSSI belum valid");

        // Kirim RSSI report ke neighbor — best-effort, tidak perlu retry.
        // Jika gagal, exchange berikutnya (RSSI_EXCHANGE_MS) akan kirim ulang.
        // Retry langsung hanya menambah NACK count saat neighbor sedang offline/restart.
        const bool ok = g_mesh.sendRssiReport(NODE_ID, myRssi);

        txCount++;
        if (!ok) failCount++;

        // Log setiap exchange agar mudah dimonitor (INFO level)
        LOG_INFO(RTAG, "RSSI report | self=%d dBm | ok=%s | fail=%lu/%lu",
                 myRssi, ok ? "Y" : "N", failCount, txCount);

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

        // ── Deferred channel sync — tidak diperlukan lagi di v5.0 ──────────────
        // processPendingChannelSync() dihapus: v5.0 WiFi-channel-sync menjamin
        // s_channelConfirmed = true sejak begin(), channel tidak pernah berubah.
        // Jika channel berubah (gateway roam), _send() handle via _updateAllPeerChannels().

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

        // ── PPG values: nolkan jika tidak ada jari (seperti monitor RS) ───────
        // Ketika jari tidak menempel, HR dan SpO2 tidak bisa diukur.
        // Setel ke 0 agar tampilan di dashboard seperti monitor rumah sakit:
        //   - Finger ON  → tampilkan nilai HR dan SpO2 aktual
        //   - Finger OFF → tampilkan 0 / garis datar
        const int8_t displayHR   = finger ? ppg.heartRate : 0;
        const float  displaySpo2 = finger ? (ppg.spo2 > 0 ? ppg.spo2 : 0.0f) : 0.0f;
        const bool   displayValid = finger ? ppg.valid : false;

        // ── Dynamic Routing Decision ──────────────────────────────────────────
        const RouteDecision dec = g_router.decide();
        const uint8_t* dstMac  = _selectDstMac(dec);

        if (dec.isDirect) directCount++;
        else              relayedCount++;

        // ── Kirim 6 IMU axis + PPG ke tujuan yang dipilih ────────────────────
        // ANTI-NACK v2 — tiga strategi:
        //
        //   #1  INTER_PKT_MS = 5ms (naik dari 2ms)
        //       Lebih menyebar paket, kurangi P(collision) dengan AP beacon.
        //       Total overhead: 6 × 5ms = 30ms per window (budget = 640ms).
        //
        //   #2  RETRY: jika NACK, tunggu random 3–8ms lalu coba sekali lagi.
        //       ESP-NOW NACK biasanya karena CCA (Clear Channel Assessment)
        //       gagal — medium sibuk sesaat. Retry dengan jitter biasanya
        //       berhasil karena collision bersifat transient.
        //
        //   #3  STAGGER: delay awal = NODE_ID × 50ms sebelum burst.
        //       Desinkronisasi transmisi antar sensor agar dua node tidak
        //       mengirim 7 paket bersamaan → menghilangkan self-collision.
        //
        //   Kombinasi #1+#2+#3: NACK diharapkan turun dari ~4.3% ke <0.5%.
        static constexpr uint8_t INTER_PKT_MS       = 5;
        static constexpr uint8_t RETRY_MIN_MS       = 3;
        static constexpr uint8_t RETRY_MAX_MS       = 8;
        // v3.2: Stagger setiap window, bukan hanya sekali.
        // 10ms × NODE_ID = overhead kecil (10-20ms) tapi cukup untuk
        // desinkronisasi transmisi antar node dan mengurangi NACK.
        static constexpr uint8_t NODE_STAGGER_MS    = 10;  // × NODE_ID, setiap window

        // #3: Stagger setiap window untuk desinkronisasi antar node
        vTaskDelay(pdMS_TO_TICKS(NODE_ID * NODE_STAGGER_MS));

        // Macro: kirim + retry 1x jika NACK
        #define SEND_WITH_RETRY(sendExpr)                                     \
            do {                                                               \
                if (!(sendExpr)) {                                             \
                    vTaskDelay(pdMS_TO_TICKS(RETRY_MIN_MS +                     \
                        (esp_random() % (RETRY_MAX_MS - RETRY_MIN_MS + 1))));  \
                    if (!(sendExpr)) nack++;                                    \
                }                                                              \
            } while (0)

        uint8_t nack = 0;

        SEND_WITH_RETRY(g_mesh.sendCsAxis(PKT_CS_AX, NODE_ID, yAx, finger, tsNow, dstMac));
        vTaskDelay(pdMS_TO_TICKS(INTER_PKT_MS));
        SEND_WITH_RETRY(g_mesh.sendCsAxis(PKT_CS_AY, NODE_ID, yAy, finger, tsNow, dstMac));
        vTaskDelay(pdMS_TO_TICKS(INTER_PKT_MS));
        SEND_WITH_RETRY(g_mesh.sendCsAxis(PKT_CS_AZ, NODE_ID, yAz, finger, tsNow, dstMac));
        vTaskDelay(pdMS_TO_TICKS(INTER_PKT_MS));
        SEND_WITH_RETRY(g_mesh.sendCsAxis(PKT_CS_GX, NODE_ID, yGx, finger, tsNow, dstMac));
        vTaskDelay(pdMS_TO_TICKS(INTER_PKT_MS));
        SEND_WITH_RETRY(g_mesh.sendCsAxis(PKT_CS_GY, NODE_ID, yGy, finger, tsNow, dstMac));
        vTaskDelay(pdMS_TO_TICKS(INTER_PKT_MS));
        SEND_WITH_RETRY(g_mesh.sendCsAxis(PKT_CS_GZ, NODE_ID, yGz, finger, tsNow, dstMac));
        vTaskDelay(pdMS_TO_TICKS(INTER_PKT_MS));
        SEND_WITH_RETRY(g_mesh.sendCsPpg(NODE_ID, yIr, displayHR,
                                          displayValid, displaySpo2,
                                          finger, tsNow, dstMac));

        #undef SEND_WITH_RETRY


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
                     "HR=%d SpO2=%.1f%% finger=%s",
                     windowCount,
                     dec.isDirect ? "DIRECT" : "RELAY",
                     dec.rssiSelf,
                     dec.rssiNeighbor,
                     relayPct,
                     g_droppedWindows,
                     dropPct,
                     nack,
                     displayHR,
                     displaySpo2,
                     finger ? "ON" : "OFF");
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

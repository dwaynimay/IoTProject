// File: firmware/lib/Watchdog/Watchdog.h

#pragma once
// =============================================================================
// Watchdog.h — System Health Monitor & Auto-Recovery
// =============================================================================
//
// Tanggung jawab modul ini:
//   1. Hardware WDT  — restart otomatis jika task hang lebih dari WDT_TIMEOUT_S
//   2. Heap monitor  — restart preventif jika free heap di bawah threshold
//   3. Stack monitor — warning jika stack watermark mendekati habis
//   4. Restart log   — catat alasan restart ke RTC memory (bertahan power cycle)
//
// CARA PAKAI:
//   // Di setup():
//   g_watchdog.begin();
//
//   // Di setiap task yang ingin dimonitor hardware WDT:
//   g_watchdog.registerTask();   // panggil sekali di awal task
//   g_watchdog.feed();           // panggil di setiap iterasi loop task
//
//   // Health check periodik (panggil dari task monitor):
//   g_watchdog.healthCheck();
//
//   // Restart terencana dengan alasan tercatat:
//   g_watchdog.triggerRestart("alasan");
//
// KONFIGURASI:
//   Semua threshold ada di dalam file ini (WDT_TIMEOUT_S, MIN_STACK_WATERMARK).
//   Threshold heap per-role (gateway vs sensor) otomatis dipilih dari NODE_ROLE.
// =============================================================================

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "Config.h"


// =============================================================================
// Konstanta Konfigurasi
// =============================================================================

// Timeout hardware WDT dalam detik.
// Task yang tidak memanggil feed() dalam waktu ini akan memicu restart.
static constexpr uint32_t WDT_TIMEOUT_S     = 30;

// Interval health check software dalam milidetik.
static constexpr uint32_t HEALTH_CHECK_MS   = 5000;

// Batas minimum stack watermark sebelum LOG_WARN dicetak.
static constexpr uint32_t MIN_STACK_WATERMARK = 512; // bytes

// Batas minimum free heap sebelum restart preventif.
//
// ⚠️  Gateway dan sensor punya budget heap yang sangat berbeda:
//   SENSOR  : tanpa WiFi aktif → free heap ~50–80 KB → threshold 20 KB aman
//   GATEWAY : WiFi AP_STA + ESP-NOW + MQTT → free heap normal ~8–12 KB
//             Threshold 20 KB akan SELALU trigger restart di gateway!
//
// NODE_ROLE di-inject oleh platformio.ini — tidak perlu ubah di sini.
#if NODE_ROLE == ROLE_GATEWAY
  static constexpr uint32_t MIN_FREE_HEAP_KB = 4;   // gateway: absolute minimum
#else
  static constexpr uint32_t MIN_FREE_HEAP_KB = 20;  // sensor: lebih konservatif
#endif


// =============================================================================
// RestartLog — Data yang bertahan di RTC memory saat restart
//
// RTC_DATA_ATTR memastikan struct ini tidak terhapus saat software restart
// (esp_restart()), sehingga kita bisa tahu alasan restart sebelumnya.
//
// Variabel g_restartLog didefinisikan di Watchdog.cpp dan di-extern di sini
// agar modul lain bisa membacanya jika diperlukan (misalnya untuk diagnostik).
// =============================================================================
struct __attribute__((packed)) RestartLog
{
    uint32_t magic;           // 0xDEADBEEF jika data valid (bukan boot pertama)
    uint32_t restart_count;   // total restart sejak pertama kali di-flash
    uint32_t last_uptime_s;   // uptime (detik) sebelum restart terakhir
    uint32_t free_heap_kb;    // free heap (KB) saat restart terakhir
    char     reason[64];      // alasan restart dalam string
};

extern RTC_DATA_ATTR RestartLog g_restartLog;


// =============================================================================
// SystemWatchdog — Class Utama
// =============================================================================
class SystemWatchdog
{
public:
    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Inisialisasi watchdog. Panggil satu kali di setup() setelah Serial.begin().
    // enableHardwareWdt = false berguna saat debugging agar tidak ada auto-restart.
    void begin(bool enableHardwareWdt = true);

    // ── Hardware WDT API ──────────────────────────────────────────────────────

    // Daftarkan task yang sedang berjalan ke hardware WDT.
    // Panggil SEKALI di awal setiap task yang ingin dimonitor.
    void registerTask();

    // "Beri makan" hardware WDT — task masih hidup, reset timer WDT.
    // Panggil di setiap iterasi loop task yang sudah registerTask().
    void feed();

    // Hapus task dari monitoring WDT. Panggil saat task akan dihentikan.
    void unregisterTask();

    // ── Health Check API ──────────────────────────────────────────────────────

    // Periksa kondisi sistem (heap, koneksi, dsb).
    // Non-blocking: hanya bekerja jika sudah lewat HEALTH_CHECK_MS.
    // Kembalikan true jika semua kondisi normal.
    bool healthCheck();

    // Periksa stack watermark task tertentu dan cetak LOG_WARN jika kritis.
    // handle = NULL untuk memeriksa task yang sedang memanggil fungsi ini.
    void checkTaskStack(const char* taskName, TaskHandle_t handle = NULL);

    // ── Restart API ───────────────────────────────────────────────────────────

    // Trigger restart terencana dengan alasan yang dicatat ke RTC memory.
    // Setelah fungsi ini dipanggil, ESP32 akan restart dalam ~500ms.
    void triggerRestart(const char* reason);

    // ── Diagnostik ───────────────────────────────────────────────────────────

    uint32_t uptimeSeconds()  const { return (millis() - _startTime) / 1000; }
    uint32_t restartCount()   const;
    uint32_t freeHeapKb()     const { return esp_get_free_heap_size() / 1024; }

private:
    uint32_t _startTime = 0;
    uint32_t _lastCheck = 0;

    void _initHardwareWdt();
    void _printRestartHistory();
};


// =============================================================================
// Instance Global
//
// Didefinisikan di Watchdog.cpp — di-extern di sini agar semua modul
// bisa akses tanpa perlu membuat instance sendiri.
// =============================================================================
extern SystemWatchdog g_watchdog;
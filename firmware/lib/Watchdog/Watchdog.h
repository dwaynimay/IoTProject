// File: firmware/lib/Watchdog/Watchdog.h

#pragma once
// =============================================================================
// SystemWatchdog — System Health Monitor & Auto-Recovery Engine
// =============================================================================
//
// Hardware  : ESP32 internal watchdog timer & RTC fast memory
// Why this implementation:
//             Combines hardware Watchdog Timer (WDT) and software task monitoring
//             to automatically restart the system upon task hangs.
//             - Monitored tasks must call feed() in each loop iteration.
//             - Monitors stack watermarks and free heap space, triggering preventive
//               restarts before Out-Of-Memory (OOM) faults occur.
//             - Retains diagnostic logs across software restarts using RTC memory
//               (g_restartLog).
//
// USAGE:
//   // In setup():
//   g_watchdog.begin();
//
//   // In a task loop to monitor:
//   g_watchdog.registerTask(); // call once at task start
//   g_watchdog.feed();         // call in each task loop iteration
//
//   // Periodic monitoring task:
//   g_watchdog.healthCheck();
//
// THREAD SAFETY:
//   Functions like feed() and registerTask() are thread-safe and can be called from
//   distinct FreeRTOS task contexts, operating directly on the underlying ESP WDT API.
// =============================================================================

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "../../include/Config.h"


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

// Minimum free heap threshold (KB) before triggering preventive restarts.
//
// ⚠️ Gateway and Sensor nodes operate under very different memory budgets:
//   SENSOR  : Without active WiFi STA/MQTT -> Free heap ~50–80 KB -> 20 KB threshold is safe.
//   GATEWAY : Running WiFi AP_STA + ESP-NOW + MQTT -> Free heap normal ~8–12 KB.
//             A 20 KB threshold would cause immediate restarts on the Gateway!
//
// NODE_ROLE is injected by platformio.ini configuration flags.
#if NODE_ROLE == ROLE_GATEWAY
  static constexpr uint32_t MIN_FREE_HEAP_KB = 4;   // gateway: absolute minimum
#else
  static constexpr uint32_t MIN_FREE_HEAP_KB = 20;  // sensor: more conservative
#endif


// =============================================================================
// RestartLog — Diagnostic log retained in RTC fast memory across soft restarts.
//
// RTC_DATA_ATTR ensures this structure is not cleared during software resets
// (esp_restart()), allowing us to retrieve the previous restart reason.
//
// Defined in Watchdog.cpp and declared extern here for system-wide access.
// =============================================================================
struct __attribute__((packed)) RestartLog
{
    uint32_t magic;           // 0xDEADBEEF if valid (not the very first boot)
    uint32_t restart_count;   // total restarts since device was flashed
    uint32_t last_uptime_s;   // uptime (seconds) before the last restart
    uint32_t free_heap_kb;    // free heap (KB) during the last restart
    char     reason[64];      // descriptive reason string for the restart
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
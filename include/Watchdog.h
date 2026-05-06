#pragma once
// =============================================================================
// Watchdog.h — System Health Monitor & Auto-Recovery
// =============================================================================

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "Config.h"  // butuh NODE_ROLE untuk threshold heap per role

// Timeout hardware WDT (detik) — task yang tidak feed dalam waktu ini akan restart
#define WDT_TIMEOUT_S       30

// Interval health check software (ms)
#define HEALTH_CHECK_MS     5000

// Batas minimum free heap sebelum restart preventif
//
// ⚠ PENTING — Gateway vs Sensor punya heap budget berbeda:
//
//   SENSOR  : tanpa WiFi STA aktif → free heap ~50-80KB → threshold 20KB OK
//   GATEWAY : WiFi AP_STA + ESP-NOW + MQTT stack → free heap normal ~8-12KB
//             Threshold 20KB akan SELALU trigger restart di gateway!
//
// Solusi: gunakan threshold berbeda per role.
//   Gateway threshold = 4KB (batas absolute, jangan sampai malloc fail)
//   Sensor threshold  = 20KB (lebih konservatif karena heap lebih besar)
//
#if NODE_ROLE == ROLE_GATEWAY
  #define MIN_FREE_HEAP_KB  4    // gateway: 4KB absolute minimum
#else
  #define MIN_FREE_HEAP_KB  20   // sensor: 20KB (heap lebih lega)
#endif

// Batas minimum stack watermark sebelum warning
#define MIN_STACK_WATERMARK 512   // byte

// ─── Data yang disimpan di RTC memory (bertahan restart) ─────────────────────
struct __attribute__((packed)) RestartLog {
    uint32_t magic;           // 0xDEADBEEF jika valid
    uint32_t restart_count;   // total restart sejak flash
    uint32_t last_uptime_s;   // uptime terakhir sebelum restart
    char     reason[64];      // alasan restart
    uint32_t free_heap_kb;    // free heap saat restart
};

// Variabel RTC — tidak direset saat restart software
extern RTC_DATA_ATTR RestartLog g_restartLog;

// ─── Watchdog class ───────────────────────────────────────────────────────────
class SystemWatchdog {
public:

    // Panggil di setup() setelah semua task dibuat
    void begin(bool enableHardwareWdt = true) {
        _startTime = millis();

        // Baca & tampilkan log restart sebelumnya
        _printRestartHistory();

        if (enableHardwareWdt) {
            _initHardwareWdt();
        }

        Serial.printf("[WDT] Watchdog aktif | HW timeout=%ds | SW check=%dms\n",
                      WDT_TIMEOUT_S, HEALTH_CHECK_MS);
        Serial.printf("[WDT] Min heap: %d KB | Min stack: %d byte\n",
                      MIN_FREE_HEAP_KB, MIN_STACK_WATERMARK);
    }

    // Panggil di setiap task loop yang didaftarkan ke WDT
    void feed() {
        esp_task_wdt_reset();
    }

    // Daftarkan task ke hardware WDT
    // Panggil di awal setiap task yang perlu dimonitor
    void registerTask() {
        esp_task_wdt_add(NULL);  // NULL = task yang memanggil
    }

    // Hapus task dari WDT (saat task selesai)
    void unregisterTask() {
        esp_task_wdt_delete(NULL);
    }

    // Health check menyeluruh — panggil dari task monitor atau loop
    // Kembalikan true jika semua OK
    bool healthCheck() {
        uint32_t now = millis();
        if (now - _lastCheck < HEALTH_CHECK_MS) return true;
        _lastCheck = now;

        bool allOk = true;
        uint32_t freeHeap = esp_get_free_heap_size() / 1024;

        // ── Cek heap ─────────────────────────────────────────────────────────
        if (freeHeap < MIN_FREE_HEAP_KB) {
            Serial.printf("[WDT] ⚠ KRITIS: Free heap %lu KB < %d KB!\n",
                          freeHeap, MIN_FREE_HEAP_KB);
            _triggerRestart("Low heap");
        }

        // ── Cek stack semua task ──────────────────────────────────────────────
        // (dilakukan oleh caller — lihat checkTaskStack())

        // ── Periodic stats ────────────────────────────────────────────────────
        uint32_t uptime_s = (now - _startTime) / 1000;
        Serial.printf("[WDT] OK | uptime=%lus | heap=%luKB | restarts=%lu\n",
                      uptime_s, freeHeap,
                      g_restartLog.magic == 0xDEADBEEF ? g_restartLog.restart_count : 0);

        return allOk;
    }

    // Cek stack watermark satu task — panggil dari task itu sendiri
    // Handle = NULL untuk task yang sedang jalan
    void checkTaskStack(const char* taskName, TaskHandle_t handle = NULL) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(handle);
        if (watermark < MIN_STACK_WATERMARK) {
            Serial.printf("[WDT] ⚠ Stack '%s' kritis: %u byte sisa!\n",
                          taskName, watermark);
        }
    }

    // Restart terencana dengan alasan tercatat
    void triggerRestart(const char* reason) {
        _triggerRestart(reason);
    }

    uint32_t uptimeSeconds() const {
        return (millis() - _startTime) / 1000;
    }

    uint32_t restartCount() const {
        return (g_restartLog.magic == 0xDEADBEEF) ? g_restartLog.restart_count : 0;
    }

private:
    uint32_t _startTime = 0;
    uint32_t _lastCheck = 0;

    void _initHardwareWdt() {
        esp_task_wdt_init(WDT_TIMEOUT_S, true);  // true = panic (restart) saat timeout
        // Daftarkan idle task di kedua core ke WDT
        esp_task_wdt_add(xTaskGetIdleTaskHandleForCPU(0));
        esp_task_wdt_add(xTaskGetIdleTaskHandleForCPU(1));
        Serial.printf("[WDT] Hardware WDT diinit (timeout=%ds)\n", WDT_TIMEOUT_S);
    }

    void _triggerRestart(const char* reason) {
        // Simpan ke RTC memory sebelum restart
        g_restartLog.magic         = 0xDEADBEEF;
        g_restartLog.restart_count = (g_restartLog.restart_count + 1);
        g_restartLog.last_uptime_s = uptimeSeconds();
        g_restartLog.free_heap_kb  = esp_get_free_heap_size() / 1024;
        strncpy(g_restartLog.reason, reason, sizeof(g_restartLog.reason) - 1);

        Serial.printf("\n[WDT] ⚠ RESTART #%lu — Alasan: %s\n",
                      g_restartLog.restart_count, reason);
        Serial.printf("[WDT]    Uptime: %lus | Heap: %luKB\n",
                      g_restartLog.last_uptime_s, g_restartLog.free_heap_kb);
        Serial.flush();
        delay(500);
        esp_restart();
    }

    void _printRestartHistory() {
        if (g_restartLog.magic != 0xDEADBEEF) {
            Serial.println("[WDT] Boot pertama (tidak ada riwayat restart)");
            return;
        }
        Serial.println("[WDT] ─── Riwayat Restart ───────────────────────");
        Serial.printf("[WDT]   Total restart: %lu\n",  g_restartLog.restart_count);
        Serial.printf("[WDT]   Restart terakhir: '%s'\n", g_restartLog.reason);
        Serial.printf("[WDT]   Uptime saat restart: %lus\n", g_restartLog.last_uptime_s);
        Serial.printf("[WDT]   Heap saat restart: %lu KB\n", g_restartLog.free_heap_kb);
        Serial.println("[WDT] ────────────────────────────────────────────");

        // Reset sebab jika reboot normal
        esp_reset_reason_t reason = esp_reset_reason();
        const char* reason_str[] = {
            "UNKNOWN","POWERON","EXT","SW","PANIC","INT_WDT",
            "TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO"
        };
        if (reason < 11) {
            Serial.printf("[WDT]   ESP reset reason: %s\n", reason_str[reason]);
        }
    }
};

// Instance global
extern SystemWatchdog g_watchdog;
// File: firmware/lib/Watchdog/Watchdog.cpp

// =============================================================================
// Watchdog.cpp — Implementasi SystemWatchdog
// =============================================================================
// Semua output log menggunakan makro LOG_* dari utils/Logger.h.
// DILARANG menggunakan Serial.print/printf secara langsung di file ini.
// =============================================================================

#include "Watchdog.h"

// Definisi storage RTC memory — harus ada tepat satu kali di seluruh project.
// Modul lain yang butuh akses g_restartLog cukup pakai 'extern' dari Watchdog.h.
RTC_DATA_ATTR RestartLog g_restartLog;

// Instance global — diakses oleh semua modul via 'extern' di Watchdog.h.
SystemWatchdog g_watchdog;


// =============================================================================
// Tag untuk Logger — konstanta lokal, tidak perlu diekspos ke luar.
// =============================================================================
static constexpr char TAG[] = "WDT";


// =============================================================================
// begin() — Inisialisasi
// =============================================================================
void SystemWatchdog::begin(bool enableHardwareWdt)
{
    _startTime = millis();

    _printRestartHistory();

    if (enableHardwareWdt)
        _initHardwareWdt();

    LOG_INFO(TAG, "Watchdog aktif | HW timeout=%ds | SW check=%dms",
             WDT_TIMEOUT_S, HEALTH_CHECK_MS);
    LOG_INFO(TAG, "Threshold: heap >= %lu KB | stack >= %lu bytes",
             MIN_FREE_HEAP_KB, MIN_STACK_WATERMARK);
}


// =============================================================================
// Hardware WDT API
// =============================================================================
void SystemWatchdog::registerTask()
{
    esp_task_wdt_add(NULL); // NULL = task yang memanggil fungsi ini
}

void SystemWatchdog::feed()
{
    esp_task_wdt_reset();
}

void SystemWatchdog::unregisterTask()
{
    esp_task_wdt_delete(NULL);
}


// =============================================================================
// healthCheck() — Pemeriksaan Berkala
//
// Menggunakan pola "early return" agar kondisi normal selesai secepat mungkin.
// Pemeriksaan yang berat (heap, stack) hanya dijalankan setiap HEALTH_CHECK_MS.
// =============================================================================
bool SystemWatchdog::healthCheck()
{
    const uint32_t now = millis();

    // Belum waktunya check — return cepat tanpa melakukan apapun
    if (now - _lastCheck < HEALTH_CHECK_MS)
        return true;

    _lastCheck = now;

    // ── Cek free heap ────────────────────────────────────────────────────────
    const uint32_t heapKb = freeHeapKb();

    if (heapKb < MIN_FREE_HEAP_KB)
    {
        // Heap kritis — catat dan restart. Tidak ada recovery yang aman.
        LOG_ERROR(TAG, "Heap kritis: %lu KB < %lu KB — trigger restart!",
                  heapKb, MIN_FREE_HEAP_KB);
        triggerRestart("Heap below minimum threshold");
        return false; // tidak akan tercapai, tapi baik untuk kejelasan
    }

    // ── Periodic stats ────────────────────────────────────────────────────────
    LOG_INFO(TAG, "OK | uptime=%lus | heap=%lu KB | restarts=%lu",
             uptimeSeconds(), heapKb, restartCount());

    return true;
}


// =============================================================================
// checkTaskStack() — Periksa Stack Watermark
// =============================================================================
void SystemWatchdog::checkTaskStack(const char* taskName, TaskHandle_t handle)
{
    const UBaseType_t watermark = uxTaskGetStackHighWaterMark(handle);

    if (watermark < MIN_STACK_WATERMARK)
    {
        LOG_WARN(TAG, "Stack '%s' kritis: %u bytes tersisa!", taskName, watermark);
    }
    else
    {
        LOG_DEBUG(TAG, "Stack '%s': %u bytes tersisa", taskName, watermark);
    }
}


// =============================================================================
// triggerRestart() — Restart Terencana
//
// Simpan alasan ke RTC memory SEBELUM restart agar bisa dibaca saat boot
// berikutnya via _printRestartHistory().
// =============================================================================
void SystemWatchdog::triggerRestart(const char* reason)
{
    // Catat ke RTC memory — bertahan saat restart software
    g_restartLog.magic         = 0xDEADBEEF;
    g_restartLog.restart_count = g_restartLog.restart_count + 1;
    g_restartLog.last_uptime_s = uptimeSeconds();
    g_restartLog.free_heap_kb  = freeHeapKb();
    strncpy(g_restartLog.reason, reason, sizeof(g_restartLog.reason) - 1);
    g_restartLog.reason[sizeof(g_restartLog.reason) - 1] = '\0'; // null-terminate

    LOG_ERROR(TAG, "RESTART #%lu | Alasan: %s",
              g_restartLog.restart_count, reason);
    LOG_ERROR(TAG, "Uptime: %lu s | Heap: %lu KB",
              g_restartLog.last_uptime_s, g_restartLog.free_heap_kb);

    // Flush Serial sebelum restart agar log terakhir tidak terpotong
    Serial.flush();
    delay(500);
    esp_restart();
}


// =============================================================================
// restartCount() — Getter aman dengan validasi magic number
// =============================================================================
uint32_t SystemWatchdog::restartCount() const
{
    return (g_restartLog.magic == 0xDEADBEEF)
           ? g_restartLog.restart_count
           : 0;
}


// =============================================================================
// _initHardwareWdt() — Setup ESP32 Hardware WDT
// =============================================================================
void SystemWatchdog::_initHardwareWdt()
{
    // panic = true: WDT timeout akan memicu panic (backtrace + restart)
    // bukan sekadar reset biasa — lebih informatif untuk debugging
    esp_task_wdt_init(WDT_TIMEOUT_S, true);

    // Daftarkan idle task di kedua core agar WDT terpicu
    // jika salah satu core hang total
    esp_task_wdt_add(xTaskGetIdleTaskHandleForCPU(0));
    esp_task_wdt_add(xTaskGetIdleTaskHandleForCPU(1));

    LOG_DEBUG(TAG, "Hardware WDT diinisialisasi (timeout=%ds)", WDT_TIMEOUT_S);
}


// =============================================================================
// _printRestartHistory() — Tampilkan Log Restart Sebelumnya
//
// Dipanggil di begin() — memberikan konteks saat troubleshooting
// mengapa perangkat restart.
// =============================================================================
void SystemWatchdog::_printRestartHistory()
{
    // Magic number tidak valid = boot pertama, tidak ada history
    if (g_restartLog.magic != 0xDEADBEEF)
    {
        LOG_INFO(TAG, "Boot pertama — tidak ada riwayat restart");
        return;
    }

    LOG_WARN(TAG, "─── Riwayat Restart ───────────────────────────");
    LOG_WARN(TAG, "Total restart  : %lu", g_restartLog.restart_count);
    LOG_WARN(TAG, "Alasan terakhir: %s",  g_restartLog.reason);
    LOG_WARN(TAG, "Uptime saat itu: %lu s", g_restartLog.last_uptime_s);
    LOG_WARN(TAG, "Heap saat itu  : %lu KB", g_restartLog.free_heap_kb);

    // Tambahkan konteks hardware reset reason dari ESP-IDF
    static const char* const RESET_REASONS[] = {
        "UNKNOWN", "POWERON", "EXT", "SW", "PANIC",
        "INT_WDT", "TASK_WDT", "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO"
    };

    const esp_reset_reason_t hwReason = esp_reset_reason();
    const char* hwReasonStr = (hwReason < 11)
                              ? RESET_REASONS[hwReason]
                              : "UNKNOWN";

    LOG_WARN(TAG, "ESP reset reason: %s", hwReasonStr);
    LOG_WARN(TAG, "────────────────────────────────────────────────");
}
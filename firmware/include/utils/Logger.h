// File: firmware/include/utils/Logger.h

#pragma once
// =============================================================================
// utils/Logger.h — Sistem Logging Terpusat
// =============================================================================
//
// CARA PAKAI:
//   #include "utils/Logger.h"
//
//   LOG_INFO ("MQTT", "Terhubung ke %s:%d", broker, port);
//   LOG_WARN ("IMU",  "Retry ke-%d gagal", attempt);
//   LOG_ERROR("PPG",  "Sensor tidak merespons!");
//   LOG_DEBUG("CS",   "Window #%lu | y[0]=%.4f", win, y[0]);
//
// OUTPUT FORMAT:
//   [  1234ms] [INFO ] [MQTT] Terhubung ke 192.168.1.7:1883
//   [  5678ms] [WARN ] [IMU ] Retry ke-2 gagal
//   [ 12345ms] [ERROR] [PPG ] Sensor tidak merespons!
//   [  9999ms] [DEBUG] [CS  ] Window #42 | y[0]=0.0312
//
// KONFIGURASI:
//   LOG_LEVEL dan LOG_ENABLE_COLOR diatur di config/features.h
//   Tidak perlu mengubah file ini untuk mengontrol verbosity.
//
// LEVEL HIERARKI:
//   DEBUG(4) > INFO(3) > WARN(2) > ERROR(1) > SILENT(0)
//   Pesan hanya tampil jika level-nya <= LOG_LEVEL yang dikonfigurasi.
// =============================================================================

#include <Arduino.h>

// Guard: pastikan LOG_LEVEL selalu terdefinisi meski features.h belum di-include
#ifndef LOG_LEVEL
  #define LOG_LEVEL 3  // default: INFO
#endif

#ifndef LOG_ENABLE_COLOR
  #define LOG_ENABLE_COLOR 0
#endif


// =============================================================================
// ANSI Color Codes — hanya aktif jika LOG_ENABLE_COLOR = 1
// =============================================================================
#if LOG_ENABLE_COLOR
  #define _LOG_COLOR_RESET  "\033[0m"
  #define _LOG_COLOR_RED    "\033[31m"
  #define _LOG_COLOR_YELLOW "\033[33m"
  #define _LOG_COLOR_CYAN   "\033[36m"
  #define _LOG_COLOR_WHITE  "\033[37m"
#else
  // Jika warna nonaktif, semua color string jadi kosong — zero cost
  #define _LOG_COLOR_RESET  ""
  #define _LOG_COLOR_RED    ""
  #define _LOG_COLOR_YELLOW ""
  #define _LOG_COLOR_CYAN   ""
  #define _LOG_COLOR_WHITE  ""
#endif


// =============================================================================
// Level Constants — dipakai oleh makro di bawah
// =============================================================================
#define _LOG_LEVEL_ERROR 1
#define _LOG_LEVEL_WARN  2
#define _LOG_LEVEL_INFO  3
#define _LOG_LEVEL_DEBUG 4


// =============================================================================
// _LOG_PRINT — Inti implementasi semua makro
//
// Kenapa pakai do { ... } while(0)?
//   Ini idiom C/C++ standar agar makro aman dipakai di dalam if/else
//   tanpa kurung kurawal, misalnya:
//     if (err) LOG_ERROR("TAG", "gagal");   ← aman, tidak ada bug tersembunyi
//
// Kenapa Serial.printf() diizinkan di sini tapi tidak di modul lain?
//   Logger.h adalah satu-satunya tempat boleh memanggil Serial secara langsung.
//   Semua modul lain wajib pakai makro LOG_* dari file ini.
//   Dengan begini, menonaktifkan semua output hanya butuh ubah LOG_LEVEL=0.
// =============================================================================
#define _LOG_PRINT(color, label, tag, fmt, ...)                         \
    do {                                                                  \
        Serial.printf(color "[%6lums] [" label "] [%-4s] " fmt           \
                      _LOG_COLOR_RESET "\n",                              \
                      (unsigned long)millis(), tag, ##__VA_ARGS__);       \
    } while (0)


// =============================================================================
// Makro Publik — gunakan ini di semua file modul
// =============================================================================

// LOG_ERROR — Kondisi fatal yang harus segera ditangani.
// Contoh: sensor tidak ditemukan, malloc gagal, koneksi putus permanen.
#if LOG_LEVEL >= _LOG_LEVEL_ERROR
  #define LOG_ERROR(tag, fmt, ...) \
      _LOG_PRINT(_LOG_COLOR_RED, "ERROR", tag, fmt, ##__VA_ARGS__)
#else
  #define LOG_ERROR(tag, fmt, ...) do {} while(0)
#endif

// LOG_WARN — Kondisi tidak normal tapi sistem masih bisa lanjut.
// Contoh: retry koneksi, queue hampir penuh, timestamp spread tinggi.
#if LOG_LEVEL >= _LOG_LEVEL_WARN
  #define LOG_WARN(tag, fmt, ...) \
      _LOG_PRINT(_LOG_COLOR_YELLOW, "WARN ", tag, fmt, ##__VA_ARGS__)
#else
  #define LOG_WARN(tag, fmt, ...) do {} while(0)
#endif

// LOG_INFO — Informasi umum alur program. Aman untuk production.
// Contoh: "Terhubung ke broker", "Task dimulai", "Window #42 dikirim".
#if LOG_LEVEL >= _LOG_LEVEL_INFO
  #define LOG_INFO(tag, fmt, ...) \
      _LOG_PRINT(_LOG_COLOR_CYAN, "INFO ", tag, fmt, ##__VA_ARGS__)
#else
  #define LOG_INFO(tag, fmt, ...) do {} while(0)
#endif

// LOG_DEBUG — Detail internal untuk development. Nonaktif di production.
// Contoh: nilai sensor mentah, isi buffer, waktu eksekusi per langkah.
#if LOG_LEVEL >= _LOG_LEVEL_DEBUG
  #define LOG_DEBUG(tag, fmt, ...) \
      _LOG_PRINT(_LOG_COLOR_WHITE, "DEBUG", tag, fmt, ##__VA_ARGS__)
#else
  #define LOG_DEBUG(tag, fmt, ...) do {} while(0)
#endif


// =============================================================================
// LOG_ONCE — Cetak pesan hanya sekali meski dipanggil berulang kali.
//
// Berguna untuk warning yang terjadi di dalam loop tapi tidak perlu
// dicetak setiap iterasi (contoh: "queue penuh" yang terjadi terus-menerus).
//
// CARA PAKAI:
//   LOG_ONCE(LOG_WARN, "MQTT", "Queue penuh, paket dibuang!");
//
// Implementasi: static bool flag per call site — zero overhead setelah pertama.
// =============================================================================
#define LOG_ONCE(level_macro, tag, fmt, ...)        \
    do {                                             \
        static bool _logged = false;                 \
        if (!_logged) {                              \
            level_macro(tag, fmt, ##__VA_ARGS__);    \
            _logged = true;                          \
        }                                            \
    } while(0)


// =============================================================================
// LOG_EVERY_N — Cetak pesan setiap N kali dipanggil.
//
// Berguna untuk log periodik di dalam loop task tanpa membanjiri Serial.
//
// CARA PAKAI:
//   LOG_EVERY_N(100, LOG_DEBUG, "CS", "buf=%d/%d", count, CS_N);
//   → Hanya cetak setiap 100 kali fungsi dipanggil.
// =============================================================================
#define LOG_EVERY_N(n, level_macro, tag, fmt, ...)      \
    do {                                                  \
        static uint32_t _call_count = 0;                  \
        if (++_call_count % (n) == 0) {                   \
            level_macro(tag, fmt, ##__VA_ARGS__);          \
        }                                                  \
    } while(0)
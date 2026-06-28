// File: firmware/lib/HealthSensors/Sensor_PPG.h
#pragma once

// =============================================================================
// ROUTER UNTUK SENSOR PPG (Berdasarkan Build Flag)
// =============================================================================
// Jika di platformio.ini ada flag: -DUSE_PPG_FINGER
// Maka otomatis akan compile versi Finger, jika tidak ada, compile versi Wrist.

#ifdef USE_PPG_FINGER
    #include "Sensor_PPG_Finger.h"
#else
    #include "Sensor_PPG_Wrist.h"
#endif
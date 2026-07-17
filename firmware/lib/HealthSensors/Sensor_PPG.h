// File: firmware/lib/HealthSensors/Sensor_PPG.h
#pragma once

// =============================================================================
// Sensor_PPG — Routing Header for PPG Sensor Implementations (Finger vs Wrist)
// =============================================================================
//
// Automatically selects the appropriate PPG sensor implementation (Finger or Wrist)
// depending on whether the USE_PPG_FINGER build flag is defined in platformio.ini.
//
// =============================================================================

#ifdef USE_PPG_FINGER
    #include "Sensor_PPG_Finger.h"
#else
    #include "Sensor_PPG_Wrist.h"
#endif
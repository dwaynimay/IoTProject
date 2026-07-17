// =============================================================================
// CS_Sensor.cpp — Static Storage Allocations for Compressive Sensing
// =============================================================================
//
// File ini mengalokasikan memori statis untuk static members dari CSPhiMatrix
// untuk mencegah linker error "undefined reference" pada compiler ESP32.
//
// =============================================================================

#include "CS_Sensor.h"

// Alokasi storage untuk singleton matrix Φ (8.192 byte di .bss / heap statis)
float CSPhiMatrix::_phi[CS_M][CS_N];
bool  CSPhiMatrix::_initialized = false;

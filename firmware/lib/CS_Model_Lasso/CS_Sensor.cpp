// =============================================================================
// CS_Sensor.cpp — Definisi static member CSPhiMatrix [DEPRECATED]
//
// File ini WAJIB ada agar linker tidak error:
//   "undefined reference to CSPhiMatrix::_phi"
//   "undefined reference to CSPhiMatrix::_initialized"
//
// Hanya berisi definisi storage — semua logic ada di CS_Sensor.h.
// =============================================================================

#include "CS_Sensor.h"

// Alokasi storage untuk singleton matrix Φ (8.192 byte di .bss / heap statis)
float CSPhiMatrix::_phi[CS_M][CS_N];
bool  CSPhiMatrix::_initialized = false;

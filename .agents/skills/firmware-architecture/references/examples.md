# Before/After Examples — Firmware Architecture

Referenced from `SKILL.md` when concrete code is needed. Four common patterns that make firmware "tangled." Examples are intentionally domain-neutral — the same shape applies to any peripheral (sensor, display, motor, radio).

---

## 1. A driver that secretly imports the comms layer

**Before** (driver knows about the wire format — wrong dependency direction):

```cpp
// Sensor_PPG.h
#include "NetPackets.h"   // <-- a driver does NOT need to know the packet format

class SensorPPG
{
public:
    bool read(PpgSample& out);   // PpgSample is defined inside NetPackets.h
};
```

Problem: this driver can no longer be reused in a project that has no networking, and it gets dragged into a recompile every time the wire protocol changes.

**After** (driver owns its struct; the comms layer converts):

```cpp
// Sensor_PPG.h — NO #include "NetPackets.h"
struct PpgSample
{
    uint32_t irRaw = 0, redRaw = 0;
    int8_t   heartRate = -1;
    float    spo2 = 0.0f;
    bool     valid = false;
};

class SensorPPG
{
public:
    bool begin();
    void update();
    bool read(PpgSample& out);
};
```

```cpp
// orchestrator.cpp — the layer that IS allowed to know about the wire format
#include "Sensor_PPG.h"
#include "NetPackets.h"

void sendHealthData(SensorPPG& ppg, NetLink& link)
{
    PpgSample s;
    ppg.read(s);

    NetPacket pkt;
    pkt.heartRate = s.heartRate;
    pkt.spo2      = s.spo2;
    link.send(pkt);
}
```

Why it matters: `Sensor_PPG.h` is now reusable in any project, and compiles/tests without the comms headers changing under it.

---

## 2. Algorithm/DSP fused into the driver (can't test without hardware)

**Before** — FFT/averaging math lives directly in the driver class, untestable without the chip:

```cpp
class PPGHandler {
public:
    bool begin();
    void update(bool isMoving);
private:
    PpgData processBatch();        // FFT, averaging — all DSP logic in here
    MAX30105   sensor;             // <-- hardware
    ArduinoFFT<float> FFT;         // <-- algorithm, fused with hardware in one class
    float vReal[BUFFER_SIZE], vImag[BUFFER_SIZE];
};
```

Problem: to test just the FFT logic, you're forced to drag along the hardware object and all its state, even though you only wanted to verify the math.

**After** — split into a pure pipeline plus a thin driver:

```cpp
// PpgDsp.h — header-only, no hardware, testable standalone
namespace ppgdsp {
class BandPass     { /* process(x) -> y, reset() */ };
class PeakEnvelope { /* ... */ };
class BeatDetector { /* ... */ };
}

// HeartRateMonitor.h — wires the DSP components, still no hardware
class HeartRateMonitor
{
public:
    bool update(float irValue, uint32_t nowMs);  // a sample goes in; knows nothing about I2C
private:
    ppgdsp::BandPass     _bandpass;
    ppgdsp::PeakEnvelope _envelope;
    ppgdsp::BeatDetector _detector;
};

// Sensor_PPG.h/.cpp — THIN driver: read register, hand off to the pipeline
class SensorPPG
{
    void update() {
        long ir = _sensor.getIR();                      // the only line touching hardware
        _hr.update(static_cast<float>(ir), millis());   // hand off to the algorithm layer
    }
private:
    MAX30105          _sensor;
    HeartRateMonitor  _hr;
};
```

Now `HeartRateMonitor` can be unit-tested on a host PC with an array of fake IR samples, no physical device required.

---

## 3. A clean orchestrator vs a tangled one

**Before** — `loop()` contains domain logic (running average, validity rules, etc.):

```cpp
void loop() {
    long ir = sensor.getIR();
    static float sumBpm = 0;
    static int count = 0;
    // ... manual BPM averaging here ...
    if (count >= 5) {
        float avgBpm = sumBpm / count;
        // ... valid/invalid logic ...
    }
}
```

**After** — `loop()` is pure orchestration; all calculation is already wrapped in methods:

```cpp
void loop() {
    bodySensor.update();
    airSensor.update();
    ppgSensor.update();

    if (ppgSensor.hasNewData()) {
        link.sendHeartData(ppgSensor.getBPM(), ppgSensor.getSpO2());
        ppgSensor.clearNewData();
    }
}
```

`begin()` for everything in `setup()`, `update()` for everything at the top of `loop()`, then a short `if` block to check-and-send — that's what keeps the orchestrator scannable as the number of peripherals grows.

---

## 4. Professional comments vs comments that clutter

**Before** — comments only translate the code, and a header comment has gone stale:

```cpp
// =============================================================================
// Sensor_MPU.h — MPU6050 driver
// =============================================================================
// Bus: Wire1, pins SDA=21, SCL=22   // <-- stale: not updated after refactor to Wire
// =============================================================================

// read sensor data
bool read(ImuSample& out);

// set sleep
void setSleep(bool enable);
```

```cpp
// begin() — init the sensor
bool SensorMPU::begin()
{
    Wire.begin(21, 22);   // start Wire
    delay(100);           // delay 100ms
    reg.write(0x00);      // write 0x00
    reg.write(0x04);      // write 0x04
}
```

Problem: the header comment lies (says Wire1, code uses Wire), and the `.cpp` comments just echo the code without explaining any decision.

**After** — comments answer questions the code can't:

```cpp
// =============================================================================
// Sensor_MPU.h — MPU6050 driver (accelerometer + gyroscope)
// =============================================================================
//
// Hardware  : MPU6050 (including clone/KW variants)
//             Bus Wire; pins configured via Pin::I2C_SDA / Pin::I2C_SCL in Config.h
//
// Why a manual implementation (no library)?
//   Common MPU6050 libraries are incompatible with clone sensors.
//   Raw I2C register access is more robust for that hardware.
//
// USAGE:
//   SensorMPU imu;
//   imu.begin();           // init & verify connection
//   imu.calibrate();       // optional; place sensor flat and still
//
//   ImuSample data;
//   if (imu.read(data)) { /* use data.accelX, data.gyroY, ... */ }
//
// THREAD SAFETY:
//   Not thread-safe. Use a mutex outside this module if accessed from
//   multiple tasks.
// =============================================================================

// Init the bus, wake the device from sleep, configure the DLPF, and verify a
// burst read. Returns false if the device does not respond or the burst read
// is short (<14 bytes) — the caller MUST check this.
bool begin();

// Toggle sleep mode. enable=true → draw drops to ~5µA (SLEEP bit, PWR_MGMT_1).
void setSleep(bool enable);
```

```cpp
bool SensorMPU::begin()
{
    Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);
    Wire.setClock(I2CClock::SPEED);
    delay(10); // 10ms is enough — no long cabling that would need extra settling

    // Wake from sleep: clear bit 6 (SLEEP) in PWR_MGMT_1.
    // The device powers up asleep per datasheet §4.28.
    Wire.beginTransmission(I2CAddr::MPU6050);
    Wire.write(Mpu6050Reg::PWR_MGMT_1);
    Wire.write(0x00);
    const uint8_t err = Wire.endTransmission();
    // ...

    // DLPF_CFG = 4 → ~21 Hz bandwidth.
    // Critical for anti-aliasing before sampling at 50 Hz (Nyquist = 25 Hz)
    // without adding compute load on the MCU.
    Wire.write(0x04);
    // ...
}
```

Key differences:
- The header describes the **contract** (usage, thread safety, design rationale) — not a list of methods.
- The register comment explains the **effect and reason** the value was chosen.
- Pins are not hardcoded in the comment — they reference `Config.h`, so the comment can't go stale on the next refactor.
- `delay(10)` is commented with *why* 10ms (not the usual 100ms).
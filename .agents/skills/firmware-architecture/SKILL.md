---
name: firmware-architecture
description: Use this skill whenever writing, reviewing, or refactoring embedded C/C++ firmware (Arduino, ESP32, STM32, nRF, Pico, bare-metal, RTOS) — driver classes, sensor/peripheral handlers, DSP or signal-processing pipelines, communication layers (BLE, mesh, WiFi, LoRa, CAN, UART protocols), or any main.cpp/setup()/loop()/RTOS-task orchestration. Trigger this for ANY firmware code task even if the user never says "modular" or "clean code" — e.g. "add a new sensor driver", "tambah driver baru", "why is my code tangled", "kenapa kode saya nyampur", "write a class for X", "refactor this file", "my hardware code is mixed with my logic", "make this reusable", or simply pasting a .h/.cpp/.ino file and asking for changes. Enforces strict layering (hardware driver vs algorithm/DSP vs protocol vs orchestration), a uniform begin()/update()/getX() public API, centralized configuration, professional why-not-what comments, and zero circular or upward dependencies — so firmware reads like a clean reusable library a senior engineer would ship, not a tangled sketch.
---

# Firmware Architecture & Modularity

Goal: every time you write or clean up embedded C/C++ firmware, the result feels like a **library you just call into** — not one big sketch where everything is tangled together. This is not about aesthetics. Correct architecture makes each layer **testable without hardware**, **reusable across projects**, and **scannable** by another engineer (or by you, next session).

Read this whole file before writing or editing any firmware. For full before/after code examples, read `references/examples.md`.

This skill is framework- and domain-agnostic. The same rules apply whether the target is an Arduino blink sketch, an ESP32 sensor node, an STM32 motor controller, or an RTOS-based gateway. Where a rule depends on the platform, it says so.

## Mental model: 4 layers, arrows point one way

Every firmware project, however small, decomposes into these 4 layers. Dependency arrows **only flow downward** — an upper layer may `#include` a lower layer, never the reverse.

```
1. Orchestration       (main.cpp / setup() / loop() / RTOS tasks:
                        WHEN to read, WHEN to send, wiring instances together)
        |
2. Communication       (BLE_Handler, MeshPackets, LoRaLink, CanBus, UartProto:
                        HOW data leaves or enters the device)
        |
3. Domain / Algorithm  (HeartRateMonitor, KalmanFilter, FilterChain, StateMachine:
                        pure computation — NO hardware, NO transport)
        |
4. Hardware Driver     (Sensor_X, Display_Y, Motor_Z:
                        register / I2C / SPI / GPIO access only)
```

**Hard rule:** layer 4 (driver) **must not** know anything about layer 2 (comms) or layer 1 (orchestration). A driver does not `#include` a packet definition, a BLE header, or `main.h`. A driver only knows how to talk to its chip and hand data back through a struct it owns (see "Each layer owns its data types").

If you find a driver that `#include`s something from layer 1 or 2, that is an **architecture bug** — always point it out to the user and offer the fix, even if they didn't ask.

### Quick way to classify a file

Ask: *"If I unplug this chip and replace it with a simulator, does this file still compile without changing a single line?"*
- Yes → layer 3 (algorithm/DSP). It MUST be testable without hardware.
- No, but only because it calls `Wire`/registers/SPI/GPIO → layer 4 (driver). That's fine, that's its job.
- No, because it needs to know a BLE/mesh/wire packet format → layer 2. Don't mix it into a driver or an algorithm.

## A uniform public API

Every driver or handler class — whatever the peripheral — exposes the same surface shape, so the orchestrator can "just call into them" without remembering a unique API per device:

```cpp
class PeripheralX
{
public:
    bool begin();              // init hardware; return false on failure — never throw/hang
    void update();             // called every loop()/tick; non-blocking
    bool read(XSample& out);   // copy latest state into an output struct

    // monitoring/debug getters — all const, all named getX()/isX()
    float getValue() const { return _value; }
    bool  isValid()  const { return _valid; }

private:
    // implementation
};
```

Consequences of this contract:
- `begin()` **always** returns `bool`, never `void`. The caller must be able to detect failure without reading logs.
- `update()` **must not block** (no long `delay()` inside) — it runs every iteration of `loop()` or every RTOS tick.
- Getter naming is consistent: `getX()` for values, `isX()`/`hasX()` for booleans — never mix `getBpm()` with `Spo2()` with `fetchTemp()` in the same codebase.
- If there are N peripherals of the same kind, keep identical method names across all their classes. Identical `begin()/update()/getX()` surfaces across every handler are exactly what lets the orchestrator read top-to-bottom without jumping into each header.

On RTOS targets, `update()` may be replaced by a dedicated task loop — but the same non-blocking, single-responsibility discipline applies: the task body reads/services the peripheral and hands data off, it does not also parse protocols or run domain math.

## Each layer owns its data types

Every layer defines its **own** structs, rather than borrowing a struct from the layer above it:

```cpp
// Sensor_PPG.h (layer 4 — driver)
struct PpgSample {
    uint32_t irRaw = 0, redRaw = 0;
    int8_t   heartRate = -1;
    float    spo2 = 0.0f;
    bool     valid = false;
};
```

The communication layer **converts** from `PpgSample` into its own packet format — the driver is never forced to know what a packet looks like:

```cpp
// in the orchestration/comms layer, NOT in the driver
PpgSample s; ppg.read(s);
NetPacket pkt;
pkt.heartRate = s.heartRate;
pkt.spo2      = s.spo2;
link.send(pkt);
```

If an output struct is used in only one place and is tiny (2–3 primitive fields), returning it directly without a named struct is fine — but once it exceeds ~4 fields or crosses a file boundary, make it a named struct in the layer that produces it.

## Algorithm/DSP layer must be testable without hardware

Best pattern: split signal/computation components into small, single-responsibility classes (header-only is convenient), each with `reset()` and `process(x) -> y`. A pipeline class wires them together. **Keep this pattern** — never re-implement domain math inside a driver just because it's "faster to write there."

Signs this layer is correct:
- No `#include <Wire.h>`, no chip-specific header, nothing that touches a hardware bus.
- The constructor takes tuning parameters as numbers (rather than reading global config in the class body) — so it can be unit-tested with different parameters.
- All state lives in members, not `static`/global.

This is the layer that benefits most from being testable on a host PC (desktop compiler) with synthetic input arrays — no physical device needed.

## Centralized configuration

All pins, bus addresses, UUIDs, timing, and calibration constants live in one config location (a `Config.h`, or namespaces like `Pin::`, `I2CClock::`, `Timing::`) — not scattered as magic numbers across drivers. A driver receives those values through the config namespace or a constructor parameter, rather than hardcoding a literal in the middle of a method.

When writing a new driver or editing an old one, if you find a meaningful literal (pin number, ADC threshold, bus address, UUID, timeout) sitting inline in the code — move it to the config location, give it a name, and reference it from there.

## The orchestrator must be dumb

`setup()`/`loop()` (or the top-level task wiring) **must not** contain DSP, parsing, or calculation. It only:
1. Declares one instance of each handler/driver.
2. Calls `.begin()` on all of them in `setup()`.
3. Calls `.update()` on all of them in `loop()`/tick.
4. Checks `.hasNewData()`/`.isValid()` and forwards to the comms layer.

If you find an `if`/`for`/math beyond a timestamp comparison (`now - last > interval`) inside `loop()`, that's a sign logic belongs in the algorithm or comms layer instead.

## Professional comments

Comments are not decoration and not a translation of code into prose. The only reason to write a comment is to answer a question the code **cannot answer by itself**: why this decision, what the trade-off is, what assumption must hold for this code to be correct.

### Core principle: explain *why*, not *what*

```cpp
// BAD — just translates the code, adds nothing:
reg.write(0x04); // write 0x04 to the CONFIG register

// GOOD — explains the engineering decision and its consequence:
// DLPF_CFG = 4 → ~21 Hz bandwidth. Critical for anti-aliasing before
// sampling at 50 Hz (Nyquist = 25 Hz) without extra MCU compute load.
reg.write(0x04);
```

If a line is self-evident (`reg.write(0x00); // wake from sleep`), one short trailing comment is enough. Long comments are reserved for non-obvious decisions — magic-number registers, hardware workarounds, or a formula with a specific physical/mathematical reason.

### Header files (.h): a contract, not a tutorial

A `.h` comment is the class's public contract — what it promises, what it requires, what it does not handle. A consistent format:

```cpp
// =============================================================================
// ClassName — One sentence: what this class does
// =============================================================================
//
// Hardware  : which chip, which bus, which pins (or "none" for pure-logic layers)
// Why this implementation: the technical reason if there's a non-obvious choice
//             (e.g. "manual register access because the common library is
//              incompatible with the clone variant of this sensor")
//
// USAGE:
//   ClassName obj;
//   obj.begin();
//   obj.update();   // call every loop()
//   auto v = obj.getValue();
//
// THREAD SAFETY:
//   State this explicitly — "not thread-safe", or "safe to read from an ISR".
//   Never leave it blank; absence of info means the caller doesn't know
//   whether a mutex is required.
// =============================================================================
```

For a public method in a `.h`, put the comment **above the declaration** (not trailing) when the explanation runs longer than ~6 words:

```cpp
// Init the bus, wake the device from sleep, and verify a burst read.
// Returns false if the device does not respond — the caller MUST check this.
bool begin();

bool isConnected() const { return _connected; } // no comment needed — name says it
```

### Section dividers in .cpp: consistent, or not at all

If a `.cpp` is long and uses section dividers (`// === Name ===`), **all** sections must use the exact same style. Don't mix `//---`, `// ***`, and `// ===` in one file. Pick one divider style per codebase and keep it:

```cpp
// =============================================================================
// methodName() — One concise sentence stating the method's purpose
// =============================================================================
// Optional paragraph for important context: input assumptions, edge cases,
// or an implementation reason not obvious from the method name alone.
// =============================================================================
```

### Keep comments in sync with code (check on every edit)

A wrong comment is more dangerous than no comment — the reader trusts it and debugs in the wrong direction.

**Rule:** every time you edit code, scan the surrounding comments. If any is no longer accurate, update it now — not later. This includes:

- A `.h` header comment naming a pin, bus, or library that has since changed.
- A register/magic-number comment whose value changed.
- A "usage" comment whose call order is no longer valid.
- A hardware description in a header (`// SDA=21, SCL=22`) that has moved or been parameterized.

When you spot a comment-vs-code mismatch during review or editing, **report it to the user specifically** (which file, which line, what's wrong) even if they didn't ask — a lying comment is a latent bug.

### What does NOT need a comment

- Self-explanatory method names (`isConnected()`, `clearCalibration()`).
- Trivial one-line getters/setters.
- `#include` lines — unless the dependency reason is non-obvious.
- Standard loops/conditions already clear from the variable names.

### Comment language

Match the language already used in the codebase. If the codebase mixes a local language with English technical terms, keep that mix — don't suddenly switch everything to English or everything to the local language when editing part of a file.

## Pre-handoff checklist

Every time you finish writing/editing a firmware file, run this checklist silently before presenting to the user — if anything fails, fix it first or explain to the user why not:

1. Drivers (those that `#include` a chip/bus header) do not `#include` anything from the comms/orchestration layer.
2. Every class has `begin() -> bool`, `update() -> void`, consistent `getX()`/`isX()` getters.
3. No meaningful literal (pin, address, calibration threshold, timeout) sits inline instead of in the config location.
4. Algorithm/DSP components could plausibly compile standalone with no hardware header.
5. The orchestrator contains no domain calculation — only method calls and timestamp checks.
6. Each data struct is owned by the layer that produces it, and converted (not borrowed) when it moves up a layer.
7. Comments explain *why*, not *what* — no comment merely restates the code.
8. Header `.h` comments are accurate: bus name, pins, and usage match the actual `.cpp` implementation.
9. If a `.cpp` uses section dividers, the format is consistent throughout the file.
10. Any comment made stale by this edit is already updated — no lying comment left behind.

If the user hands you code that violates any point above, **always tell them specifically which point and why**, then offer the fix — don't silently leave it. If it's not the focus of their request, mention it briefly and then focus on the main task.

## See also

- `references/examples.md` — full before/after examples: a driver caught importing a packet definition, DSP bleeding into a driver, and a clean orchestrator.
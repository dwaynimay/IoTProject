# IoTProject Firmware And Server Bug Audit Ledger

Audit baseline: 2026-07-20 (Asia/Jakarta)

## Audit Protocol

Each finding must include an ID, phase, severity, status, symptom, evidence,
impact, reproduction or verification method, and recommended action. Findings
remain `candidate` until their assigned phase validates the full runtime path.

Severity levels:

- `Critical`: prevents core operation, corrupts memory/data, or can permanently
  disconnect a node.
- `High`: causes major data loss, incorrect routing, crashes, or failed recovery.
- `Medium`: degrades reliability or leaves an important path untested.
- `Low`: documentation, maintainability, or tooling issue with limited runtime
  impact.

Status values: `candidate`, `confirmed`, `fixed`, `verified`, `deferred`, or
`not-a-bug`.

## Baseline

- Git branch: `main`
- Git commit: `5ae9e70521f50f79f7597c9ace646b789197e379`
- Source worktree: clean before this ledger was created
- Firmware platform: Espressif32 `7.0.1`, Arduino ESP32 framework
  `3.20017.241212`, board `esp32dev`
- Server runtime: Python `3.13.14`
- Server test result: `112 passed, 1 failed, 1 warning`
- Server syntax result: `compileall` passed, with one invalid-escape warning in
  a backup capture script
- Ruff result: `341` findings (`314` automatically fixable), including archived
  notebooks and support tools
- Black result: `67` files would be reformatted, `6` unchanged
- Mypy result: `133` errors in `48` files; many are import-root or missing-stub
  configuration issues, mixed with real type defects

Production firmware build matrix:

| Environment | Build | RAM | Flash |
| --- | --- | ---: | ---: |
| `node_imu` | PASS | 57,932 bytes (17.7%) | 778,101 bytes (59.4%) |
| `node_ppg` | PASS | 57,248 bytes (17.5%) | 779,161 bytes (59.4%) |
| `node_gateway` | PASS | 46,932 bytes (14.3%) | 793,925 bytes (60.6%) |

## Findings

### BASE-001: Full-pipeline test uses an obsolete NodeState constructor

- Phase: 0 / server baseline
- Severity: Medium
- Status: confirmed
- Symptom: `test_integration_full_pipeline` fails before exercising the pipeline.
- Evidence: `server/tests/test_full_pipeline.py:56` passes `node_id`, while
  `server/apps/reconstruct/node_state.py:53` requires `group_id`, `imu_node_id`,
  and `ppg_node_id`.
- Impact: the only full pipeline integration test provides no regression
  protection for reconstruction, storage, and REST output.
- Verification: run `python -m pytest server/tests -q`.
- Recommended action: update the test fixture to the current cross-node contract,
  then verify asynchronous dispatch deterministically.

### BASE-002: Firmware mesh tests and documentation reference stale topology

- Phase: 0 / test baseline
- Severity: Medium
- Status: confirmed
- Symptom: mesh tests expect one neighbor and document environments that do not
  exist.
- Evidence: `firmware/test/test_mesh_routing.cpp:69` expects
  `maxNeighborsPerNode == 1`, while `firmware/include/config/tuning.h:143` defines
  `2`; the test and root README reference `node_sensor_a`/`node_sensor_b`, while
  production environments are `node_imu`/`node_ppg`. Phase 4 compilation also
  confirms `test_mesh_sensor_n1` and `test_mesh_sensor_n2` fail because
  `manual_mesh_routing.cpp:132` calls the old eight-argument `sendCsPpg()` API;
  production now requires the ninth `mean` argument.
- Impact: mesh tests cannot reliably validate current production configuration.
- Verification: compare `firmware/platformio.ini` with the test assertions and
  README build commands.
- Recommended action: align tests and docs with production environment names and
  replace unconditional `TEST_PASS()` integration placeholders with assertions.

### BASE-003: Server quality gates mix production source with archives and tools

- Phase: 0 / tooling baseline
- Severity: Low
- Status: confirmed
- Symptom: Ruff, Black, and mypy produce large noisy reports that obscure runtime
  defects.
- Evidence: Ruff reports 341 findings and scans archived notebooks; mypy reports
  133 errors including import-root and missing-stub failures.
- Impact: maintainers cannot use a clean quality-gate signal during bug fixes.
- Verification: run Ruff, Black, and mypy using the current `server/pyproject.toml`.
- Recommended action: define production/test/tool scopes explicitly, configure
  the package import root, and triage real type errors separately from missing
  third-party stubs.

### MESH-001: Beacon RSSI callback filters the wrong Wi-Fi frame class

- Phase: 3
- Severity: Critical
- Status: confirmed
- Symptom: sensor RSSI remains `RoutingCfg::RSSI_UNKNOWN` (`-127`) and relay
  selection never receives a valid self-to-gateway measurement.
- Evidence: `firmware/lib/EspNowMesh/EspNowMesh.cpp:633` accepts only
  `WIFI_PKT_DATA`, while ESP-NOW uses vendor-specific action frames.
- Impact: observed IMU/PPG RSSI `-127`, direct-only fallback, and broken channel
  drift detection.
- Verification: ESP-IDF 4.4 documents ESP-NOW payloads as vendor-specific action
  frames, which are management rather than data frames. Historical captures show
  the resulting `-127` RSSI symptom. A corrected-filter hardware assertion remains
  required after the fix.
- Recommended action: validate management/action frames and their ESP-NOW header
  before extracting source MAC and RSSI.

### MESH-002: Wi-Fi timeout marks fallback channel as confirmed

- Phase: 3
- Severity: Critical
- Status: confirmed
- Symptom: a sensor that cannot join Wi-Fi falls back to channel 1 but starts its
  sender tasks as if the gateway channel were known.
- Evidence: `firmware/lib/EspNowMesh/EspNowMesh.cpp:142-171` sets channel 1 on
  timeout and then unconditionally sets `s_channelConfirmed = true`.
- Impact: sensor can remain permanently isolated when the gateway uses another
  channel.
- Verification: source-path analysis confirms the fallback is marked known and
  that no channel sweep exists. Promiscuous reception only sees the currently
  tuned channel, so an off-channel gateway cannot trigger the recovery callback.
- Recommended action: retain an unconfirmed state and run a bounded recovery
  strategy instead of treating fallback as discovery success.

### MESH-003: Broadcast and unicast sends share an uncorrelated callback result

- Phase: 3
- Severity: High
- Status: confirmed
- Symptom: an RSSI report can log `ok=N` even when the peer receives a report.
- Evidence: `firmware/lib/EspNowMesh/EspNowMesh.cpp:454-459` returns from a
  broadcast without waiting, while all send callbacks write one global result and
  semaphore at `firmware/lib/EspNowMesh/EspNowMesh.cpp:511-540`. A unicast timeout
  at lines 472-479 also releases the mutex without invalidating its eventual
  callback, which can then satisfy the next send after its initial semaphore drain.
- Impact: stale callbacks can complete the wrong transaction, corrupt ACK/NACK
  metrics, and violate ESP-NOW send sequencing.
- Verification: overlap fast gateway beacons with node unicast traffic and log
  callback MAC versus the MAC of the active transaction.
- Recommended action: serialize every send through callback completion and match
  callback MAC/generation to the active transaction.

### CONFIG-001: Configured IMU peer MAC does not match the tested IMU hardware

- Phase: 3 / peer identity
- Severity: Critical
- Status: confirmed
- Symptom: IMU-to-PPG RSSI reports succeed, while every PPG-to-IMU RSSI report
  returns `ok=0` in the same end-to-end run.
- Evidence: `firmware/include/config/hardware.h:56` configures IMU as
  `F4:2D:C9:6F:5C:40`, but the captured IMU startup at
  `server/data/Backup_Pengujian_EndToEnd/serial_and_mqtt_raw.log:48` reports its
  actual STA MAC as `68:09:47:77:19:04`. The PPG target is selected from the
  stale constant at `EspNowMesh.cpp:328-330`; log lines 52-93 repeatedly show
  IMU reports `ok=1` and PPG reports `ok=0`.
- Impact: the PPG cannot send neighbor RSSI reports or relay traffic to the IMU,
  the IMU never learns the PPG neighbor link, and bidirectional mesh routing is
  impossible on the recorded hardware set.
- Verification: the asymmetric runtime result exactly follows the configured
  destinations: the IMU sends to the correct PPG MAC, while the PPG sends to an
  absent IMU MAC. Re-read the current board's STA MAC before applying a fix in
  case the hardware has since been replaced.
- Recommended action: update/provision peer identities from the actual boards,
  print and verify all role-to-MAC mappings at startup, and reject deployment if
  a configured role cannot complete a bidirectional identity handshake.

### MESH-005: Channel recovery mutates the radio and peer table outside send locking

- Phase: 3 / channel recovery
- Severity: High
- Status: confirmed
- Symptom: a pending channel update can run while another task is inside an
  ESP-NOW send transaction.
- Evidence: `_send()` is serialized by `s_sendMutex` at
  `EspNowMesh.cpp:418-479`, but `processPendingChannelSync()` changes the Wi-Fi
  channel, deletes/re-adds the gateway, and modifies all peers at lines 597-624
  without acquiring that mutex.
- Impact: recovery can race an active send, producing peer-not-found, wrong-channel
  transmission, false NACKs, or corrupted peer iteration exactly when the link is
  already degraded.
- Verification: source synchronization analysis; stress hardware by forcing
  channel-update flags while CS and RSSI tasks transmit.
- Recommended action: perform channel and peer-table changes under the same
  transport lock, quiesce in-flight sends first, and check every mutation result.

### MESH-006: ESP-NOW callback and promiscuous setup errors are ignored

- Phase: 3 / initialization
- Severity: Medium
- Status: confirmed
- Symptom: `begin()` can report success even when send/receive callbacks or RSSI
  monitoring were not installed.
- Evidence: return values from `esp_now_register_send_cb()`,
  `esp_now_register_recv_cb()`, `esp_wifi_set_promiscuous()`, and
  `esp_wifi_set_promiscuous_rx_cb()` are ignored at `EspNowMesh.cpp:199-200` and
  `231-236`; the radio channel-set result at line 170 is also ignored.
- Impact: failures become silent `ok=N`, timeouts, an empty receive queue, or
  permanent RSSI `-127`, while logs misleadingly state that initialization
  succeeded.
- Verification: inject or force an invalid ESP-NOW/Wi-Fi state and observe that
  the current return path remains true.
- Recommended action: check and log each `esp_err_t`, unwind partial
  initialization, and return false for mandatory transport components.

### MESH-007: ESP-NOW task callbacks use FreeRTOS ISR-only queue APIs

- Phase: 3 / callback execution
- Severity: Medium
- Status: confirmed
- Symptom: send and receive callbacks use `xSemaphoreGiveFromISR()`,
  `xQueueSendFromISR()`, and `portYIELD_FROM_ISR()` even though they are not ISR
  callbacks.
- Evidence: `EspNowMesh.cpp:537-539` and `562-564` use ISR APIs. Espressif's
  ESP-IDF 4.4 ESP-NOW guide states that both callbacks execute in the
  high-priority Wi-Fi task and recommends posting minimal data to a queue.
- Impact: scheduler semantics are incorrect and portability is fragile; callback
  work also runs in a latency-sensitive Wi-Fi task.
- Verification: compare callbacks with the framework version's documented
  execution context and instrument task/ISR state on hardware.
- Recommended action: use nonblocking task-context queue/semaphore APIs, keep the
  callback minimal, and process accounting/logging in a lower-priority task.

### MESH-008: Receive queue overflow silently discards mesh packets

- Phase: 3 / receive buffering
- Severity: Medium
- Status: confirmed
- Symptom: when the 20-entry receive queue is full, packets disappear without a
  counter, warning, or backpressure signal.
- Evidence: `EspNowMesh.cpp:562-564` ignores the return value from
  `xQueueSendFromISR()`; `getQueueMetrics()` reports occupancy but not drops.
- Impact: bursts can lose RSSI reports, CS axes, or routed packets and make radio
  failures indistinguishable from application-side congestion.
- Verification: pause the consumer and inject more than 20 non-beacon packets.
- Recommended action: check the enqueue result, maintain per-type drop counters,
  expose high-water marks, and size or prioritize the queue from measured load.

### MESH-004: Routed packet length is not validated against received frame length

- Phase: 2 / packet contract (revisit in Phase 4 routing)
- Severity: High
- Status: confirmed
- Symptom: a truncated `ROUTED_CS` frame can request a copy beyond valid received
  bytes.
- Evidence: `firmware/lib/Routing/MeshRouting.cpp:135-154` validates `innerLen`
  against buffer capacity but not `raw.len >= sizeof(RoutedCsHeader) + innerLen`.
- Impact: out-of-bounds read, malformed reconstruction, or gateway instability.
- Verification: route a truncated frame whose declared `innerLen` exceeds the
  available payload.
- Recommended action: reject frames unless declared and actual lengths match.

### GATEWAY-001: Wi-Fi outage timer is not reset after recovery

- Phase: 4
- Severity: Medium
- Status: confirmed
- Symptom: after one recovered outage, a later brief outage can trigger an
  immediate gateway restart.
- Evidence: `firmware/src/main.cpp:190-197` initializes `wifiDownSince` on failure
  but has no connected-state branch that resets it.
- Impact: unnecessary reboot and temporary loss of ESP-NOW/MQTT connectivity.
- Verification: disconnect/reconnect Wi-Fi twice with each outage shorter than 30
  seconds. Source analysis confirms the function-local static timestamp is only
  reachable inside the disconnected branch and therefore cannot be cleared after
  a successful reconnect.
- Recommended action: reset the outage timestamp whenever Wi-Fi is connected.

### ROUTE-001: Relay selection ignores the source-to-relay link

- Phase: 4 / route decision
- Severity: High
- Status: confirmed
- Symptom: a node selects a relay solely because that relay reports a stronger
  link to the gateway, even when the source cannot reliably reach the relay.
- Evidence: `DynamicRouter.cpp:197-202` compares only `neighbor->gateway` RSSI
  against `self->gateway`. No state or packet field measures `self->neighbor`,
  and `RssiReportPacket` only carries the reporting node's gateway RSSI.
- Impact: the router can deliberately move traffic from a usable direct link to
  an unusable first hop, causing retries and complete window loss.
- Verification: place the relay near the gateway but shield it from the source;
  report a high relay-to-gateway RSSI and observe that `decide()` still selects it.
- Recommended action: include bidirectional source-relay quality and recent send
  success in the path cost; select relay only when both hops are valid and the
  end-to-end cost beats direct by a hysteresis margin.

### ROUTE-002: Failed relay delivery has no end-to-end fallback

- Phase: 4 / forwarding reliability
- Severity: High
- Status: confirmed
- Symptom: a source retries a selected first hop twice, but never retries direct;
  the relay forwards each packet only once and ignores the forwarding result.
- Evidence: the `SEND_WITH_RETRY` macro at `task_cs_sender.cpp:366-395` retries the
  same `dstMac`; it does not change route. `main.cpp:118-120` calls
  `forwardRoutedCs()` without checking its boolean, and that function performs one
  gateway send at `EspNowMesh.cpp:367-378`.
- Impact: one failed axis forward prevents the complete six-axis IMU window from
  publishing; MAC ACK at the first hop also cannot prove gateway delivery.
- Verification: force relay-to-gateway loss after source-to-relay ACK and compare
  source success logs with gateway window completion.
- Recommended action: add an application-level window/packet ACK, bounded relay
  retries, and direct fallback when the relay path fails or its failure score rises.

### ROUTE-003: Packet identities are not bound to ESP-NOW source MACs

- Phase: 4 / route integrity
- Severity: High
- Status: confirmed
- Symptom: routing trusts node IDs inside packet bytes rather than the sender MAC.
- Evidence: `taskSensorReceiver` reads `PacketHeader::nodeId` and forwards it at
  `main.cpp:94-120` without comparing `raw.srcMac`; gateway handlers similarly use
  inner node IDs for buffers/topics. `_routeRoutedCs()` does not require outer
  `originalNodeId` to equal the inner header node ID or verify `relayNodeId` against
  `raw.srcMac`.
- Impact: malformed, stale, or injected packets can be attributed to another node,
  contaminate its IMU accumulation buffer, or create false relay audit metadata.
- Verification: send a valid frame from one configured MAC with another node ID,
  and send a routed wrapper whose outer and inner identities disagree.
- Recommended action: maintain a role-to-MAC map at receive time, reject identity
  mismatches, and validate outer relay/original IDs against both MAC and inner header.

### ROUTE-004: DynamicRouter locking does not protect readers

- Phase: 4 / concurrency
- Severity: Medium
- Status: confirmed
- Symptom: RSSI values and timestamps are written in a critical section but read
  concurrently without entering that section.
- Evidence: updates use `taskENTER_CRITICAL()` at `DynamicRouter.cpp:84-93` and
  `135-142`; `decide()`, validity helpers, getters, and status logging read the same
  non-volatile fields without the lock at lines 151-240.
- Impact: decisions can combine RSSI and timestamps from different updates; the
  C++ data race is undefined behavior across the ESP32's two cores.
- Verification: run RSSI updates on core 1 while repeatedly deciding/logging on
  core 0 under ThreadSanitizer in a host model or with version counters on hardware.
- Recommended action: snapshot all routing state under the same critical section,
  then make the decision from that immutable snapshot outside the lock.

### ROUTE-005: Claimed N-node routing collapses all neighbors into one RSSI slot

- Phase: 4 / topology
- Severity: Medium
- Status: confirmed
- Symptom: reports from any valid neighbor overwrite one `_rssiNeighbor`, but a
  relay decision always returns the first `_neighborNodeId`; destination MAC
  selection is hard-coded by local node role.
- Evidence: `DynamicRouter.cpp:117-145` accepts every configured neighbor into one
  value, while lines 201-202 choose only `_neighborNodeId`. `_selectDstMac()` at
  `task_cs_sender.cpp:165-174` ignores `RouteDecision::nextHopNodeId` entirely.
- Impact: adding more neighbors can route using one neighbor's RSSI to a different
  neighbor's MAC. The advertised N-node behavior is unsafe beyond the current
  two-sensor topology.
- Verification: configure two non-gateway neighbors, update only the second with a
  strong RSSI, and inspect the selected ID and actual destination MAC.
- Recommended action: store state per neighbor, rank candidates by complete path
  cost, and resolve destination MAC from `nextHopNodeId` through one validated map.

### ROUTE-006: The relay threshold is not true hysteresis

- Phase: 4 / route stability
- Severity: Medium
- Status: confirmed
- Symptom: one dBm of noise around the 5 dBm threshold can alternate every window
  between direct and relay.
- Evidence: `DynamicRouter.cpp:199-216` selects relay at `diff >= 5` and direct at
  `diff < 5`, with no remembered route or separate enter/exit thresholds. Archived
  controlled runs contain three final recovery-phase transmissions still marked
  RELAY while DIRECT was expected as scripted RSSI state changed.
- Impact: route flapping increases retries, duplicates, latency variation, and
  partial IMU windows during changing radio conditions.
- Verification: alternate the RSSI difference between 4 and 5 dBm and call
  `decide()` repeatedly.
- Recommended action: retain current route, use separate relay-enter and relay-exit
  margins plus a dwell period, and expire each neighbor independently.

### GATEWAY-002: Wi-Fi reconnect does not resynchronize ESP-NOW channel state

- Phase: 4 / gateway recovery
- Severity: Critical
- Status: confirmed
- Symptom: after the gateway reconnects to Wi-Fi on a different AP channel, its
  ESP-NOW peer records and internal `s_channel` remain on the startup channel.
- Evidence: `_connectWifi()` can run again from `NetworkMqtt::tryReconnect()` at
  `Network_Mqtt.cpp:123-134`; STA/AP channel is selected at lines 158-193.
  `g_mesh.setGatewayChannel()` is called only once during setup at `main.cpp:300-309`
  and no reconnect path notifies the mesh.
- Impact: beacon transmission and peer sends can fail due to channel mismatch;
  sensors cannot recover because their off-channel discovery path is also broken
  by MESH-001/MESH-002.
- Verification: reconnect the gateway to the same SSID after its AP channel changes
  and compare `WiFi.channel()`, mesh channel, beacon callbacks, and peer channels.
- Recommended action: emit a Wi-Fi/channel-change event, serialize
  `setGatewayChannel()` with sends, update every peer, and test recovery across
  channels without rebooting sensors.

### GATEWAY-003: Idle MQTT publish failure permanently loses the message

- Phase: 4 / gateway publish queue
- Severity: High
- Status: confirmed
- Symptom: a message received while the drain loop is idle is removed from the
  queue and discarded if its immediate publish fails.
- Evidence: `task_mesh_handler.cpp:216-220` dequeues and calls `publish()` but only
  logs false; unlike lines 197-207, it never returns the message to the queue.
- Impact: transient MQTT/TCP failure silently loses a complete PPG or reconstructed
  IMU window despite the surrounding code claiming failed messages are retried.
- Verification: enqueue one message into an empty queue and force the next
  PubSubClient publish call to fail.
- Recommended action: use one publish helper for both drain paths that requeues on
  failure, or peek then remove only after successful publish.

### GATEWAY-004: Retained online status has no MQTT Last Will

- Phase: 4 / gateway presence
- Severity: Medium
- Status: confirmed
- Symptom: the retained gateway status can remain `online` after an ungraceful
  disconnect or power loss.
- Evidence: `_publishOnlineStatus()` publishes retained `online` at
  `Network_Mqtt.cpp:240-245`, but `_connectMqtt()` at lines 200-207 uses connect
  overloads without Last Will topic/payload despite comments claiming an offline
  will exists.
- Impact: server/UI health monitoring can report a dead gateway as online.
- Verification: power off the gateway after connection and inspect the retained
  status topic after broker keepalive expiry.
- Recommended action: connect with retained QoS Last Will payload `offline`, then
  publish retained `online` only after successful session establishment.

### GATEWAY-005: FreeRTOS task creation failures are ignored

- Phase: 4 / lifecycle
- Severity: Medium
- Status: confirmed
- Symptom: setup logs that a node is ready even if a mandatory sender, receiver,
  beacon, handler, MQTT, or monitor task could not be allocated.
- Evidence: all `xTaskCreatePinnedToCore()` return values at `main.cpp:247-318` are
  ignored.
- Impact: heap pressure can leave a partially initialized device that appears
  healthy but never sends, relays, publishes, or monitors data.
- Verification: lower available heap or increase a task stack until task creation
  returns an error and observe setup continue.
- Recommended action: check every return value, log the failed task and free heap,
  then restart or enter an explicit degraded state.

### SENSOR-001: PPG IR and Red values come from different sample instants

- Phase: 1 / PPG acquisition
- Severity: High
- Status: confirmed
- Symptom: every `PpgMeasurement` combines an IR value and a Red value captured
  at different FIFO updates.
- Evidence: `Sensor_PPG_Wrist.cpp:88-89` and the finger implementation call
  `MAX30105::getIR()` followed by `getRed()`. In the installed SparkFun library,
  both getters independently call `safeCheck(250)`, and each successful
  `safeCheck` requires `check()` to fetch new sensor data before returning.
- Impact: the ratio-of-ratios calculation compares non-simultaneous channels,
  which can bias or destabilize SpO2 even when heart-rate detection from IR is
  otherwise usable.
- Verification: instrument consecutive FIFO sequence numbers or raw pairs and
  show that Red advances after IR in one `update()` call. This requires connected
  MAX30102 hardware.
- Recommended action: call `check()` once, then consume paired IR/Red values from
  the same FIFO slot using `getFIFOIR()`/`getFIFORed()` and `nextSample()`.

### SENSOR-002: PPG timeout re-emits stale data as a successful acquisition

- Phase: 1 / PPG acquisition
- Severity: High
- Status: confirmed
- Symptom: when `getIR()` times out and returns zero, `update()` exits without
  clearing state, while the following `read()` still returns `true` and copies
  the previous IR, Red, BPM, SpO2, contact, and validity values.
- Evidence: `Sensor_PPG_Wrist.cpp:84-110` returns immediately for `ir == 0`;
  `Sensor_PPG_Wrist.cpp:248-264` has no freshness/error state and always returns
  `true`. The finger implementation is structurally identical.
- Impact: sensor disconnection or I2C stalls can be encoded as apparently fresh
  CS samples and can preserve a false `fingerOn`/valid measurement indefinitely.
- Verification: acquire a valid reading, disconnect or shut down the MAX30102,
  then call `update()` and `read()` and observe repeated old values with success.
- Recommended action: make acquisition return a fresh/error result, invalidate
  state on timeout, count consecutive failures, and only push a CS sample when a
  new paired FIFO sample was consumed.

### SENSOR-003: Sensor sampling pauses during synchronous ESP-NOW transmission

- Phase: 1 / acquisition scheduling
- Severity: High
- Status: confirmed
- Symptom: the same `CS_TX` task reads sensors, fills a 64-sample window, encodes
  it, and synchronously sends the result before reading the next sample.
- Evidence: `task_cs_sender.cpp:257-408` performs the entire sequence in one
  loop. Each unicast `_send()` can block for 50 ms and is retried once; an IMU
  window sends six packets with five additional 5 ms gaps.
- Impact: IMU sample spacing can gain a worst-case gap above 600 ms per window;
  PPG can gain a gap above 100 ms. CS reconstruction assumes a regularly sampled
  window, so these gaps distort time-domain signals and can discard sensor FIFO
  history under radio loss.
- Verification: toggle a GPIO around acquisition and record intervals with a
  logic analyzer while the gateway is reachable, unreachable, and relayed.
- Recommended action: decouple periodic acquisition from encoding/transmission
  with a bounded sample queue or double buffer; timestamp samples/windows and
  define an explicit overrun policy.

### SENSOR-004: Runtime I2C failures have no recovery path

- Phase: 1 / sensor lifecycle
- Severity: High
- Status: confirmed
- Symptom: startup failures restart the node, but a sensor that fails after
  startup is never reinitialized or marked disconnected.
- Evidence: `Sensor_MPU.cpp:86-92` simply returns false after a failed burst and
  leaves `_connected=true`; the PPG `update()` path likewise never changes
  `_connected` after `begin()`. `main.cpp:241-269` handles only initial failure.
- Impact: a transient bus fault or loose cable leaves the node alive but unable
  to recover without an external reset; PPG additionally exposes stale data via
  SENSOR-002.
- Verification: disconnect/reconnect SDA or sensor power after successful boot
  and observe that acquisition does not resume.
- Recommended action: track consecutive I2C failures, invalidate output, recover
  the bus/device with bounded backoff, and escalate to a controlled restart only
  after repeated failed recovery.

### SENSOR-005: MPU6050 configuration and transaction errors are partly ignored

- Phase: 1 / IMU driver
- Severity: Medium
- Status: confirmed
- Symptom: the driver assumes default full-scale ranges and accepts failed
  configuration/register-address writes until a later byte-count check.
- Evidence: `Sensor_MPU.cpp:44-47`, `52-56`, and `196-201` ignore
  `endTransmission()` results; `ACCEL_CONFIG` and `GYRO_CONFIG` are never written,
  while conversion constants assume +/-2 g and +/-250 deg/s.
- Impact: a retained or externally changed MPU configuration yields incorrectly
  scaled physical values; bus faults are diagnosed late and inconsistently.
- Verification: preconfigure non-default full-scale ranges before MCU restart or
  inject NACKs on the register-address phase.
- Recommended action: explicitly configure and read back all scale/filter
  registers, and reject every failed I2C transaction at its source.

### SENSOR-006: Heart rate above 127 BPM is silently clipped

- Phase: 1 / PPG output contract
- Severity: Medium
- Status: confirmed
- Symptom: valid detector output in the 128-150 BPM range is reported as exactly
  127 BPM.
- Evidence: the beat detector permits 400 ms intervals (150 BPM) and
  `HeartRateMonitor::isValid()` accepts BPM below 200, but
  `Sensor_PPG_Wrist.cpp:261` and the finger equivalent constrain output to 127
  because `PpgMeasurement::heartRate` is `int8_t`.
- Impact: exercise/tachycardic readings are corrupted before packet encoding and
  cannot be recovered by the server.
- Verification: feed the DSP a stable synthetic waveform above 127 BPM and read
  `PpgMeasurement::heartRate`.
- Recommended action: use `uint8_t` with a separate validity sentinel/flag, or a
  wider signed type consistently through firmware and server schemas.

### SENSOR-007: IMU temperature field is always fabricated as zero

- Phase: 1 / IMU output contract
- Severity: Low
- Status: confirmed
- Symptom: `ImuMeasurement::tempC` is always `0` even though the burst already
  reads the MPU6050 temperature registers.
- Evidence: `Sensor_MPU.cpp:102` assigns zero and `_burstRead()` discards bytes
  6-7 at lines 208-209.
- Impact: downstream code cannot distinguish an unimplemented temperature from
  a real 0 degrees C sample.
- Verification: inspect any successful IMU read.
- Recommended action: parse and convert the temperature value, or remove the
  field/mark it explicitly unavailable throughout the contract.

### SENSOR-008: Sensor and DSP behavior has no host-side regression suite

- Phase: 1 / test coverage
- Severity: Medium
- Status: confirmed
- Symptom: sensor test sketches require physical hardware and only print values;
  there are no assertions for FIFO pairing, freshness, disconnect recovery,
  filter behavior, beat timing, clipping, or sample scheduling.
- Evidence: searches under `firmware/test`, `firmware/test_sketches`, and
  `server/tests` find only manual sketches instantiating `SensorPPG`/`SensorMPU`;
  no tests instantiate the reusable classes in `PpgDsp.h`.
- Impact: the Phase 1 defects can compile successfully and regress unnoticed.
- Verification: run the current automated firmware test discovery and inspect
  the manual-only sensor sketches.
- Recommended action: isolate I2C/FIFO access behind injectable interfaces and
  add deterministic native tests for DSP and acquisition state machines, plus a
  small hardware-in-loop disconnect test.

## Phase 1 Hardware Gaps

- No USB serial sensor device was available during this audit; only Bluetooth
  COM ports were present.
- Register/FIFO semantics were verified against the exact SparkFun library copy
  installed by PlatformIO, but electrical behavior and timing still require the
  hardware verification steps listed above.

## Phase 1 Build Verification

- `node_imu`: PASS
- `node_ppg` (production wrist configuration): PASS
- `test_imu`: PASS
- `test_ppg` (finger configuration): PASS
- `test_imu_raw_vs_cs`: PASS
- `test_ppg_raw_vs_cs`: PASS
- PlatformIO emitted a non-fatal `I2C_BUFFER_LENGTH` macro redefinition warning
  between SparkFun MAX3010x 1.1.2 (`32`) and ESP32 `Wire.h` (`128`) in the two
  raw-vs-CS builds.

### PACKET-001: Gateway CS JSON serialization can write beyond its payload buffer

- Phase: 2 / packet serialization
- Severity: Critical
- Status: confirmed
- Symptom: a CS packet containing a float whose formatted representation exceeds
  the remaining JSON capacity can advance the output pointer beyond
  `MqttMessage::payload` and pass a negative remaining length to `snprintf`.
- Evidence: `MeshRouting.cpp:367-387` and `420-446` repeatedly apply
  `p += w; rem -= w` without checking whether `snprintf` returned `w >= rem`.
  `_writeFloatArray()` at lines 455-464 only checks `rem > 15`, although values
  such as `FLT_MAX` require substantially more than 15 characters with `%.4f`.
  A subsequent negative `int rem` is converted to a very large `size_t` by
  `snprintf`.
- Impact: malformed, corrupted, or spoofed ESP-NOW CS data can cause gateway
  memory corruption, crash/restart, or publish malformed JSON.
- Verification: route a syntactically valid `CS1AxisPacket` whose measurements
  contain `FLT_MAX` under AddressSanitizer or a guarded output buffer.
- Recommended action: use a checked append helper that treats `w < 0 || w >= rem`
  as failure, reject non-finite/out-of-range packet values before formatting, and
  return `DROPPED` rather than publishing truncated JSON.

### PACKET-002: IMU and PPG timestamps are not in a shared clock domain

- Phase: 2 / timestamp contract
- Severity: High
- Status: confirmed
- Symptom: IMU and PPG packets carry each node's local `millis()` value even
  though the sensors run on separate ESP32 devices with independent boot times.
- Evidence: `task_cs_sender.cpp:350-395` stamps packets with local `millis()`.
  `EspNowMesh::sendTimeSync()` exists at `EspNowMesh.cpp:301`, but no production
  code calls it; no sensor handler consumes `TIME_SYNC` or applies an offset.
- Impact: server-side cross-node window pairing cannot know whether IMU and PPG
  measurements represent the same physical interval. Boot-order offsets and
  clock drift can silently pair unrelated windows and degrade multimodal ML or
  physiological interpretation.
- Verification: boot the two sensor nodes several seconds apart and compare
  packet timestamps for simultaneously generated windows.
- Recommended action: implement periodic gateway time sync with sensor offsets
  and wrap-safe correction, or assign a gateway receive/window sequence and pair
  on that explicit shared identity.

### PACKET-003: Invalid node IDs alias the gateway IMU buffer for node 1

- Phase: 2 / packet identity
- Severity: High
- Status: confirmed
- Symptom: every packet with `nodeId` outside 1-2 is accumulated in buffer index
  zero, the same state used by legitimate node 1.
- Evidence: `MeshRouting.h:69-72` returns index zero for all invalid IDs, and
  `_routeCsAxis()` uses that index without first rejecting the header node ID.
- Impact: malformed or spoofed packets can reset, overwrite, or complete node 1's
  six-axis window and publish measurements under an untrusted node ID.
- Verification: interleave node 1 axis packets with an otherwise valid axis
  packet whose header has `nodeId=3`.
- Recommended action: make node lookup return an invalid result, enforce the
  configured node-role map, and validate source MAC against header identity.

### PACKET-004: The firmware-server Phi verification tool is broken by imports

- Phase: 2 / CS synchronization tooling
- Severity: Medium
- Status: confirmed
- Symptom: the documented `python -m tools.verify_phi` command exits with an
  import error before printing the matrix.
- Evidence: `tools/verify_phi.py:20` imports `cs.hadamard`; importing package
  `cs` executes `cs/__init__.py`, which imports `router`, then `core.config`, while
  `core/__init__.py` imports back from partially initialized `cs`.
- Impact: maintainers cannot use the advertised check after changing `CS_N`,
  `CS_M`, seed, LCG, or row selection, increasing the chance of silent
  firmware-server matrix divergence.
- Verification: from `server/`, run
  `.venv/Scripts/python.exe -m tools.verify_phi`; it fails with a circular import.
- Recommended action: remove the `core`/`cs` package cycle, keep configuration in
  a dependency-neutral module, and make the verification command part of CI.

### PACKET-005: Binary packet ABI has no compile-time contract checks

- Phase: 2 / packet layout
- Severity: Medium
- Status: confirmed
- Symptom: packet structs are sent by raw in-memory representation, but no
  `static_assert` locks their size, field offsets, enum width, float width, or
  ESP-NOW payload ceiling.
- Evidence: `MeshPackets.h` uses packed structs and `EspNowMesh.cpp:338-364`
  transmits them with `sizeof`, but searches find no packet layout assertions.
  The header summary is already stale, listing axis/PPG sizes as 136/142 bytes
  while their current declarations correctly total 140/146 bytes.
- Impact: a compiler, field, or configuration change can silently alter the wire
  format or exceed limits without failing the build.
- Verification: compare the top-of-file size table with declarations at
  `MeshPackets.h:204-221`.
- Recommended action: add fixed-width wire fields plus `static_assert` checks for
  every transmitted size/offset and the routed maximum; avoid relying on C++
  `bool` representation in the wire ABI.

### PACKET-006: CS and packet behavior lacks an automated cross-language test

- Phase: 2 / regression coverage
- Severity: Medium
- Status: confirmed
- Symptom: Python tests validate Python-generated Phi and reconstruction, while
  firmware sketches only print diagnostics and do not assert encoded golden
  vectors, packet bytes, gateway aggregation, or serializer bounds.
- Evidence: `test_cs_hadamard.py` checks shape, determinism, and Python residuals;
  `test_cs_compression.cpp` is a one-shot manual sketch with serial output. There
  are no automated `MeshRouting` serializer/packet-layout tests.
- Impact: cross-language drift and packet regressions can pass both firmware
  builds and server tests independently.
- Verification: inspect current test assertions and note that none compare a
  firmware encoded vector or packet byte fixture against Python.
- Recommended action: define shared golden input/mean/Phi/y fixtures, assert
  exact packet sizes and decoded fields, and fuzz/truncation-test gateway routing.

### SERVER-001: Cross-node windows are paired despite unrelated timestamps

- Phase: 5 / cross-node pairing
- Severity: Critical
- Status: confirmed
- Symptom: an IMU and PPG payload are dispatched together even when their sensor
  timestamps differ by many seconds.
- Evidence: `NodeState._try_dispatch()` computes spread at
  `node_state.py:151-160` but explicitly accepts the pair when it exceeds
  `TS_SPREAD_TOLERANCE_MS`. Firmware timestamps are independent per-device
  `millis()` clocks (PACKET-002), so even a small numeric spread does not prove the
  same physical interval. A read-only experiment paired IMU `ts=2000` with PPG
  `ts=9000` while logging that 7000 ms exceeded the 500 ms tolerance.
- Impact: reconstruction, vitals, quality assessment, and ML combine unrelated
  movement and pulse windows under one output timestamp, producing scientifically
  invalid multimodal records without rejecting them.
- Verification: feed valid group payloads with a 7000 ms spread; processor dispatch
  still occurs once.
- Recommended action: establish a shared clock/window identity at firmware or
  gateway level, pair by that identity with a bounded nearest-neighbor queue, and
  quarantine rather than dispatch pairs outside tolerance.

### SERVER-002: Single-slot pairing silently overwrites unmatched windows

- Phase: 5 / pairing buffer
- Severity: High
- Status: confirmed
- Symptom: if two IMU windows arrive before one PPG window, the first IMU window
  is silently replaced and the newest IMU is paired with whichever PPG arrives.
- Evidence: `node_state.py:71-74` stores one dictionary per modality, and
  `on_imu()`/`on_ppg()` unconditionally assign those slots at lines 103 and 123.
  The pairing experiment submitted IMU timestamps 1000 then 2000 followed by PPG
  9000; only `(2000, 9000)` reached the processor.
- Impact: asymmetric packet loss or jitter causes silent window loss and arbitrary
  cross-modal pairing; no counter reveals overwritten data.
- Verification: send two validated messages of one modality before its partner and
  inspect the sole processor call.
- Recommended action: maintain bounded per-modality queues keyed by shared window
  ID/time, match nearest valid pairs, and expose unmatched/expired/overwritten counts.

### SERVER-003: Validator does not require or validate CS means and vital metadata

- Phase: 5 / payload contract
- Severity: High
- Status: confirmed
- Symptom: payloads missing every `mean_ax`...`mean_gz` or `mean_ir` pass validation;
  malformed means also pass until processor conversion fails.
- Evidence: required fields at `validator.py:79-83` omit all mean fields, `spo2`,
  and `ppg_valid`; validation only checks measurement vectors. Processor defaults
  missing means to zero at `processor.py:75` and `87`, or calls `float()` on an
  unchecked supplied value. Experiments confirmed missing means and string/NaN
  means return `(True, [])` from the validator.
- Impact: because firmware compresses mean-centered signals, a missing mean shifts
  the full reconstructed signal to the wrong baseline; malformed metadata can
  crash a processor job after its pair has already been removed.
- Verification: validate an otherwise correct payload with no mean fields, then
  compare reconstructed absolute signal against one containing the true means.
- Recommended action: define strict typed schemas for every firmware JSON field,
  require finite bounded means/vitals, reject booleans as numbers, and version the
  payload contract.

### SERVER-004: Invalid payloads can advance monotonic timestamp state

- Phase: 5 / validation state
- Severity: High
- Status: confirmed
- Symptom: a payload that later fails length or finite validation still changes the
  node's last accepted timestamp and can make subsequent valid data look replayed.
- Evidence: `_run_layers()` updates `_MonotonicityTracker` at
  `validator.py:375-387` before length/finite checks at lines 389-395. Experiment:
  valid `ts=1000`, invalid-length `ts=2000`, then valid `ts=1500`; the last payload
  was rejected against poisoned `prev=2000`.
- Impact: one malformed or malicious message can suppress valid windows until the
  source timestamp catches up or crosses the reboot heuristic.
- Verification: reproduce the three-payload sequence above with one registry.
- Recommended action: perform all stateless validation first and commit timestamp
  state only after the complete payload is accepted.

### INGEST-001: Malformed JSON values can permanently stop MQTT ingestion

- Phase: 5 / MQTT callback safety
- Severity: Critical
- Status: confirmed
- Symptom: syntactically accepted JSON such as a complete payload with `ts=NaN`
  or `ts=Infinity` raises inside validation instead of being rejected normally.
- Evidence: Python `json.loads()` accepts these constants by default;
  `validator.py:381` calls `int(ts)` without finite checks, producing `ValueError`
  or `OverflowError`. `listener._on_message()` only catches decode/JSON errors at
  lines 145-150. Installed Paho has `suppress_exceptions=False` and re-raises
  callback exceptions; `main_app.py:65-81` then logs worker death but never restarts it.
- Impact: one malformed MQTT message can leave FastAPI/dashboard running while all
  subsequent sensor ingestion has stopped.
- Verification: publish a schema-complete payload whose timestamp is JSON `NaN`;
  the read-only validator experiment reproduced the uncaught exception.
- Recommended action: reject non-standard JSON constants, require a dictionary,
  wrap the complete callback boundary, isolate per-message failures, and supervise
  the MQTT worker with health reporting and bounded restart backoff.

### INGEST-002: Initial MQTT connection failure is never retried in all-in-one mode

- Phase: 5 / listener lifecycle
- Severity: High
- Status: confirmed
- Symptom: if the broker is unavailable during server startup, the MQTT background
  thread exits while the HTTP application continues serving.
- Evidence: `listener.py:200-201` calls synchronous `client.connect()` before
  `loop_forever()`. `_run_mqtt_thread()` catches the resulting exception at
  `main_app.py:68-81`, logs it, and returns; no supervisor recreates the thread.
- Impact: service startup order or a temporary broker outage produces a permanently
  stale dashboard until the entire server is restarted.
- Verification: start all-in-one server with the broker stopped, then start the
  broker and observe that no new MQTT connection is attempted.
- Recommended action: run a supervised reconnect loop with exponential backoff,
  expose worker health, and make readiness depend on ingestion state where required.

### SERVER-005: Per-group processor jobs can complete out of order

- Phase: 5 / asynchronous reconstruction
- Severity: High
- Status: confirmed
- Symptom: consecutive windows for the same group run concurrently and may be
  stored/notified in reverse order.
- Evidence: every pair is submitted independently to a shared two-worker pool at
  `node_state.py:167-173`, with no per-group serialization. A read-only experiment
  delayed window 1 and observed completion order `[2, 1]`. Both jobs also mutate
  the same `_timing` dict at `processor.py:96-100`, and share a `QualityAssessor`
  whose counters have no lock.
- Impact: WebSocket charts can move backward, timing averages race, latest-record
  semantics become nondeterministic, and quality counters can be inaccurate.
- Verification: make window 1 processing slower than window 2 and record storage
  or notifier completion order.
- Recommended action: preserve FIFO execution per group, separate parallelism
  across groups, and protect or remove shared mutable statistics.

### SERVER-006: Processor queue is unbounded and has no overload policy

- Phase: 5 / backpressure
- Severity: High
- Status: confirmed
- Symptom: validated pairs are submitted to `ThreadPoolExecutor` without checking
  queue depth, age, or processing capacity.
- Evidence: module-global `_PROCESSOR_POOL` at `node_state.py:30-32` uses the
  executor's unbounded internal work queue; `_try_dispatch()` always submits at
  lines 170-173. Futures are not retained or cancelled.
- Impact: slow OMP, SQLite, or ML work can grow memory indefinitely and emit very
  stale results long after their health-monitoring value has expired.
- Verification: replace processor with a blocking function and feed pairs faster
  than two workers can consume them while monitoring memory/work queue size.
- Recommended action: use a bounded queue with explicit drop/coalesce policy,
  overload metrics, maximum result age, and per-group FIFO workers.

### SERVER-007: Processor failures permanently consume paired inputs

- Phase: 5 / failure recovery
- Severity: High
- Status: confirmed
- Symptom: once submitted, both modality buffers are cleared and `windows_done` is
  incremented even if reconstruction, storage, or notification later fails.
- Evidence: `node_state.py:162-173` consumes the pair before execution;
  `_run_processor()` catches all errors at lines 175-189 but only logs them. There
  is no retry, dead-letter record, failed-window counter, or rollback.
- Impact: transient SQLite/ML/code errors silently create permanent gaps while
  window numbering implies success.
- Verification: inject a processor that raises and inspect buffers,
  `windows_done`, storage events, and retry behavior.
- Recommended action: track submitted/succeeded/failed states separately, persist
  failure metadata, retry only safe transient stages, and make storage writes
  idempotent by group/window identity.

### INGEST-003: Sensor data uses at-most-once MQTT delivery

- Phase: 5 / transport contract
- Severity: High
- Status: confirmed
- Symptom: sensor windows sent during brief broker/network disruption can vanish
  without broker acknowledgement or replay.
- Evidence: gateway PubSubClient publishes with its default QoS 0 and server
  subscribes via `client.subscribe(topic)` at `listener.py:138-141`, also defaulting
  to QoS 0. No durable local spool or application sequence reconciliation exists.
- Impact: TCP/session interruption can lose already reconstructed gateway windows,
  and the server cannot detect which sequence is missing.
- Verification: interrupt connectivity immediately after a QoS 0 publish and
  compare firmware window counters with server records.
- Recommended action: use QoS 1 where supported, persistent sessions plus a bounded
  gateway spool, and explicit window IDs/deduplication on the server.

### SERVER-008: Stale pairing buffers expire only when another payload arrives

- Phase: 5 / state cleanup
- Severity: Medium
- Status: confirmed
- Symptom: if one modality stops completely, its unmatched buffer is never expired
  or reported until some later IMU/PPG callback enters the same group.
- Evidence: `_expire_stale_buffers()` is called only inside `on_imu()` and
  `on_ppg()` at `node_state.py:100-125`; there is no timer/maintenance sweep. It
  also uses wall-clock `time.time()` rather than monotonic elapsed time.
- Impact: stale health state remains invisible, and system clock adjustments can
  delay or prematurely trigger expiry.
- Verification: submit one modality, stop all group traffic for more than three
  seconds, and inspect that the buffer remains populated.
- Recommended action: run periodic monotonic-time cleanup and emit missing-modality
  health events/counters independently of new traffic.

### QUALITY-001: Sparsity metric measures reconstructed samples, not DCT coefficients

- Phase: 6 / signal quality
- Severity: High
- Status: confirmed and reproduced
- Symptom: a DCT-sparse signal can be reported as fully dense, making the displayed
  sparsity metric unrelated to the compressed-sensing representation it documents.
- Evidence: `quality.py:288-290` counts nonzero values in time-domain `x_arr`.
  A deterministic one-coefficient DCT signal reported `1.0` while its actual DCT
  sparsity was `1/64 = 0.015625`.
- Impact: quality dashboards and any future sparsity threshold can misdiagnose
  healthy reconstruction and obscure genuinely non-sparse inputs.
- Recommended action: compute sparsity from the recovered coefficient vector, or
  transform `x_hat` back to the configured basis before counting; add a one-tone
  DCT regression test.

### STORAGE-001: Historical window and quality counts have incompatible units

- Phase: 6 / persistence statistics
- Severity: High
- Status: confirmed and reproduced
- Symptom: `total_windows` undercounts after a process restart while low/critical
  counts can increase by up to seven for one window.
- Evidence: `storage.py:354` counts distinct `window_num`, which restarts from one,
  while lines 355-356 count quality flags per signal row. Two inserted windows with
  repeated number 1 yielded `total_windows=1`; one seven-signal critical window
  yielded `critical_count=7`.
- Impact: dashboard rates and historical health summaries are mathematically
  inconsistent and can move backward after restart.
- Recommended action: persist a session/boot ID plus stable window identity and
  aggregate quality once per window using its worst flag.

### ML-001: Storage-based inference skips the realtime IR normalization

- Phase: 6 / ML input contract
- Severity: High
- Status: confirmed and reproduced
- Symptom: the same stored IR samples produce different model features depending
  on whether inference is realtime or replay/batch.
- Evidence: `adapter.py:132-134` defines division by `180.88`, but line 147 assigns
  raw `signal_rows['ir']` instead of `_to_g('ir')`. Input `180.88` became `1.0` in
  `from_processor()` and remained `180.88` in `from_storage_rows()`.
- Impact: replay validation and batch predictions are not comparable to production
  predictions and may select different stress labels.
- Recommended action: use `_to_g('ir')` and add adapter parity tests for all seven
  signals and metadata fields.

### ML-002: Probability columns are trusted to match config label order

- Phase: 6 / ML model contract
- Severity: High
- Status: confirmed structural risk
- Symptom: reordering labels in JSON silently attaches each probability to the
  wrong semantic class.
- Evidence: `engine.py:205-212` enumerates config labels over `predict_proba()`
  columns; load validation checks feature count but never validates labels against
  `model.classes_`. Active models expose numeric classes `[0..3]` and `[0,1]`, so
  the pickle contains no directly verifiable semantic names.
- Impact: a fall probability can be presented as walking/sitting without any load
  error if the manifest order drifts from training encoding.
- Recommended action: persist an explicit class-value-to-label mapping from the
  training encoder and reject model/config mismatches at load time.

### ML-003: Model discovery depends on the process working directory

- Phase: 6 / ML startup
- Severity: High
- Status: confirmed by test
- Symptom: the server can start with zero active models when launched from the
  `server` directory instead of repository root.
- Evidence: `main_app.py:94` scans relative path
  `server/apps/ml_inference/models/`. Running the Phase 6 suite from `server`
  produced `Registry scan: folder ... tidak ada` and failed `test_cek_ml.py` with
  model count zero, although both model files exist.
- Impact: deployment/service-manager working-directory differences silently
  disable all ML inference.
- Recommended action: resolve model paths from `Path(__file__)`, fail startup when
  required models are absent, and test startup from multiple working directories.

### API-001: Destructive database endpoints are exposed without authentication

- Phase: 6 / API security
- Severity: Critical
- Status: confirmed
- Symptom: any network client that reaches the server can purge historical data or
  delete all records for a selected node.
- Evidence: `maintenance.py:11-47` exposes unauthenticated POST purge and DELETE
  node-data routes; `main_app.py:145` binds all interfaces and `app.py:43` allows
  every CORS origin.
- Impact: accidental browser requests or a LAN attacker can cause irreversible
  monitoring-history loss.
- Recommended action: require authentication/authorization, restrict CORS and bind
  address by deployment config, add CSRF protection where cookies are used, and
  require explicit confirmation/audit logging for destructive operations.

### SERVER-009: Validation failures bypass live metrics and event notification

- Phase: 6 / observability
- Severity: Medium
- Status: confirmed
- Symptom: malformed sensor payloads are persisted but do not increment the API's
  validation counter and are not pushed to connected event clients.
- Evidence: `node_state.py:94-116` calls only `storage.log_event()`; the counter is
  incremented exclusively inside `notifier.py:109-110`, which is never called on
  those paths.
- Impact: `/api/metrics` can report zero validation errors during an active corrupt
  data stream, delaying diagnosis.
- Recommended action: route all events through one persistence-and-notification
  service and test DB, metric, and WebSocket side effects together.

### WS-001: Slow clients can stall broadcasts and create unbounded pending work

- Phase: 6 / WebSocket delivery
- Severity: High
- Status: confirmed
- Symptom: one slow socket delays every later client while producer threads keep
  scheduling additional broadcasts without backpressure.
- Evidence: `hub.py:46-50` awaits clients sequentially; lines 62-70 discard the
  Future returned by `run_coroutine_threadsafe()` and enforce no queue limit,
  timeout, or per-client serialization.
- Impact: under load, messages become stale, memory grows, and concurrent sends to
  the same socket may fail unpredictably.
- Recommended action: use bounded per-client queues with one sender task per
  socket, delivery timeout/drop policy, and observable queue/drop counters.

### FRONTEND-001: Event details are inserted as unsanitized HTML

- Phase: 6 / browser security
- Severity: High
- Status: confirmed vulnerable sink
- Symptom: event type, node ID, and detail are interpolated into `innerHTML`.
- Evidence: `ui.js:67-75` renders event fields without escaping. Event details can
  include rejected payload values and are stored by the server.
- Impact: once an attacker-controlled event reaches this renderer, script-capable
  markup can execute in an operator's dashboard session.
- Recommended action: construct elements with `textContent`, validate presentation
  fields, and add DOM-XSS tests with HTML and event-handler payloads.

### FRONTEND-002: Events WebSocket cannot recover independently

- Phase: 6 / browser reliability
- Severity: High
- Status: confirmed
- Symptom: if only `/ws/events` disconnects, event updates stop permanently while
  the dashboard still appears connected through `/ws/stream`.
- Evidence: `ws.js:77-88` defines only `onmessage` for the event socket. The
  reconnect guard at line 13 refuses reconnection while either socket object still
  exists. Snapshot messages from both endpoints are also ignored.
- Impact: critical alerts and event history can silently disappear until a full
  page reload or stream-socket failure.
- Recommended action: manage each socket lifecycle independently, process initial
  snapshots, expose per-channel status, and test asymmetric disconnects.

### API-002: Activity history returns quality flags in restart-unsafe order

- Phase: 6 / API semantics
- Severity: Medium
- Status: confirmed
- Symptom: an endpoint labelled activity/ML history returns quality labels and
  orders them by a counter that resets after restart.
- Evidence: `nodes.py:67-83` selects `quality_flag` and orders by
  `window_num ASC`; no ML result is persisted in the windows schema.
- Impact: the UI can show `OK/CRITICAL` as user activity and interleave old/new
  sessions incorrectly.
- Recommended action: persist model name, semantic label, confidence, timestamp,
  and session identity; rename quality history separately and order by timestamp
  plus row ID.

## Phase 6 Verified Server And Dashboard Path

- Runtime ML uses two deployed pickle models: a four-class IMU model with 40
  features and a two-class PPG model with 10 features.
- Config labels currently have the same cardinality as numeric model classes, but
  semantic order cannot be verified from numeric `classes_` alone.
- Controlled experiments reproduced the sparsity error, realtime/storage IR scale
  mismatch, repeated-window undercount, and per-signal critical overcount.
- Source review traced API deletion exposure, validation metric bypass, WebSocket
  backpressure/lifecycle failures, activity semantic mismatch, and the DOM-XSS
  sink. These paths lack focused integration/browser tests.

## Phase 6 Build And Test Verification

- `pytest tests/test_quality.py tests/test_storage.py tests/test_ml_engine.py
  tests/test_cek_ml.py`: `73 passed, 1 failed`.
- The one failure is the working-directory-dependent model scan documented as
  ML-003, not a missing model artifact.
- Existing passing tests assert only `low_quality_count >= 1`, so they do not catch
  the per-signal overcount reproduced in STORAGE-001.
- Phase 6 introduced no production source changes; only this audit ledger changed.

### INTEGRATION-001: Initial MQTT connection failure permanently disables ingestion

- Phase: 7 / startup recovery
- Severity: High
- Status: confirmed and reproduced
- Symptom: when the broker is unavailable during server boot, REST/API remains
  healthy but MQTT ingestion never retries during that process lifetime.
- Evidence: `listener.py:200-201` performs synchronous `connect()` before
  `loop_forever()`. `main_app.py:65-81` catches the resulting exception and lets
  the sole daemon worker exit; no supervisor restarts it. Fault injection against
  closed port 65500 showed one start attempt, a logged worker crash, and a live
  `/api/status` endpoint two seconds later.
- Impact: service orchestration can report the server healthy while every sensor
  window is silently absent if Mosquitto starts a little later than FastAPI.
- Recommended action: retry initial connection with bounded exponential backoff,
  supervise worker liveness, and expose MQTT connected/last-message/retry state in
  readiness and metrics endpoints.

## Phase 7 Integration And Fault-Injection Results

- A real local Mosquitto -> MQTT listener -> group pairing -> OMP -> quality ->
  SQLite -> both ML models -> REST run processed 3 complete windows into 21 signal
  rows. Last HR was 72 and both models returned predictions; the server remained
  alive throughout.
- An invalid one-sample PPG vector created a persisted `VALIDATION_ERROR`, while
  `/api/metrics` still returned `total_val_errors=0`, reproducing SERVER-009 across
  MQTT, state, storage, and API boundaries.
- Blocking both processor workers while submitting 100 pairs left 98 jobs queued,
  reproducing SERVER-006's unbounded backlog rather than applying backpressure.
- An injected processor exception left `windows_done=1`, both input buffers empty,
  and no failure event, reproducing SERVER-007's permanent silent loss.
- A 350 ms fake slow WebSocket client delayed the following fast client by 363.6
  ms, reproducing WS-001's sequential head-of-line blocking.
- Starting with a closed MQTT port reproduced INTEGRATION-001. In contrast, after
  one successful connection, stopping and restarting an isolated Mosquitto on port
  18884 did recover: connect callback count rose to two and windows advanced from
  one to two.
- No ESP32 serial ports were present. Radio/channel/relay fault injection and
  validation of corrected `ok`/RSSI behavior therefore remain hardware-only Phase
  8 checks.

## Phase 7 Verification Summary

- Nominal local end-to-end pipeline: PASS, 3 windows / 21 persisted signal rows.
- Validation fault visibility: FAIL, event persisted but metric remained zero.
- Processor overload policy: FAIL, 98 of 100 submitted jobs queued unbounded.
- Processor failure recovery: FAIL, consumed window was neither retried nor
  recorded as failed.
- WebSocket slow-client isolation: FAIL, fast client delayed by 363.6 ms.
- Broker absent at startup: FAIL, API alive with permanently dead MQTT worker.
- Broker restart after successful connection: PASS, listener reconnected and
  resumed ingestion.
- Production firmware/server source files were not changed during Phase 7.

## Phase 8A Firmware Fixes

- `CONFIG-001` fixed: `MacAddr::NODE_IMU` now matches the captured hardware MAC
  `68:09:47:77:19:04`, so PPG RSSI reports target the actual IMU peer.
- `MESH-001` fixed in code: promiscuous RSSI filtering now accepts
  `WIFI_PKT_MGMT`, the frame class used by ESP-NOW vendor action frames, instead
  of rejecting everything except `WIFI_PKT_DATA`.
- `MESH-002` fixed in code: AP timeout no longer marks fallback channel 1 as
  confirmed, and sensors sweep channels 1-13 with a 1.2-second dwell until the
  gateway beacon confirms the active channel.
- `MESH-003` fixed: each send records its expected destination MAC, consumes the
  broadcast callback, and ignores callbacks that are late or belong to another
  transaction.
- `MESH-005`, `MESH-006`, and `MESH-007` fixed: channel/peer mutation is serialized
  with the send mutex, ESP-NOW/promiscuous callback setup errors fail startup, and
  callbacks use normal non-blocking FreeRTOS APIs appropriate to Wi-Fi task
  context rather than ISR-only APIs.
- `MESH-004`, `PACKET-001`, `PACKET-003`, and `PACKET-005` fixed: routed length is
  checked against the received frame, JSON writes reject truncation before pointer
  advancement, invalid node IDs no longer alias node 1, and packet ABI/maximum
  routed size are compile-time assertions.
- `ROUTE-003` partially fixed: direct CS packets are bound to the configured source
  MAC, and routed CS validates relay MAC plus outer/inner node identity. Legacy
  combined and heartbeat packets are not yet identity-bound.
- `GATEWAY-001` through `GATEWAY-005` fixed for the audited paths: Wi-Fi outage
  duration resets after recovery, channel changes resynchronize ESP-NOW peers,
  idle publish failures are requeued, MQTT now registers a retained `offline`
  Last Will, and every production task creation is checked.

## Phase 8A Verification

- Production `node_imu`: PASS, RAM 57,940 bytes, flash 779,153 bytes.
- Production `node_ppg`: PASS, RAM 57,256 bytes, flash 780,217 bytes.
- Production `node_gateway`: PASS, RAM 46,932 bytes, flash 796,229 bytes.
- `test_mesh_gateway`: PASS build with the new packet ABI assertions and routing
  validation.
- The existing MAX3010x/ESP32 Wire dependency still emits an
  `I2C_BUFFER_LENGTH` redefinition warning; it did not fail these builds.
- No ESP32 serial ports were present, so corrected `ok=Y`, non-`-127` RSSI,
  gateway channel recovery, Last Will, and relay identity rejection still require
  three-device hardware verification in Phase 8D.

## Phase 8B Server Core Fixes

- MQTT ingestion now rejects non-object JSON, contains exceptions across the
  complete dispatch callback, persists `INGEST_ERROR` where possible, and retries
  initial broker failures with bounded exponential backoff.
- Validator schemas now require firmware means and PPG vital metadata, validate
  scalar metadata as finite/range-correct values, and commit timestamp state only
  after stateless checks pass. Invalid packets can no longer poison subsequent
  monotonicity decisions or crash on NaN/Infinity timestamps.
- The one-slot modality buffers and shared unordered executor were replaced by
  bounded per-group deques and one bounded FIFO worker per group. Pairing now uses
  server arrival spread because the two physical sensor clocks are not synchronized;
  mismatch, expiration, buffer overflow, queue overload, and processor failures are
  persisted and published as explicit events.
- Event persistence/notifier failures are contained so observability failures do
  not terminate the per-group processor worker.
- Reconstruction sparsity is now measured on orthonormal DCT coefficients, matching
  the OMP basis, rather than nonzero time-domain samples.
- SQLite rows carry a unique process `session_id`; existing databases migrate with
  a `legacy` default. Node statistics group by session and window and count the
  worst quality flag once per window instead of once per signal.
- Stored IR values now receive the same model-training normalization as live
  processor values. Model discovery is anchored to `main_app.py` rather than the
  caller working directory, and model/manifest class counts and textual class order
  are validated before inference.

## Phase 8B Verification

- Targeted validator, NodeState, quality, storage, ML, registry, and full-pipeline
  suite: `111 passed`.
- Full server suite including Hadamard/OMP tests: `120 passed`; after adding legacy
  schema and event-storage fault tests, focused NodeState/storage suite: `30 passed`.
- Fault tests verify FIFO processing without overwrite, worker survival after
  processor and event-storage exceptions, idle stale-pair cleanup, explicit bounded
  overload behavior, invalid-timestamp tracker isolation, and old-schema migration.
- `git diff --check`: PASS; only existing Windows LF-to-CRLF conversion warnings.
- Remaining server work is Phase 8C: API query ordering/contracts and WebSocket
  slow-client isolation. Hardware and complete end-to-end regression remain Phase 8D.

## Phase 8C API, WebSocket, And Dashboard Fixes

- WebSocket fan-out is concurrent across clients, bounded by a per-client timeout,
  and serialized by a per-socket lock. One slow client no longer delays fast
  clients, while concurrent window publishes cannot overlap writes to one socket.
- Thread-safe publish futures are observed so asynchronous failures are logged;
  snapshot database reads run outside the asyncio event loop.
- SQLite-backed REST handlers now use FastAPI's synchronous worker execution rather
  than blocking the event loop. Activity history uses a public storage query ordered
  by server timestamp and row ID, so window-number resets across sessions cannot
  reorder the timeline. Segment duration now includes the configured 640 ms final
  window instead of an arbitrary two-second fallback.
- Dashboard server counters are protected by a shared lock across group workers and
  read as one consistent snapshot by the metrics endpoint.
- The browser reconnects if either stream or event socket closes, suppresses duplicate
  reconnect timers, rejects malformed WebSocket JSON safely, consumes initial
  snapshots, merges REST refresh state, and removes nodes absent from the server.
- Event rendering now uses DOM `textContent` instead of interpolating persisted event
  fields into `innerHTML`, closing a stored-XSS path.
- Destructive maintenance endpoints are disabled unless `ADMIN_API_TOKEN` is set and
  require a constant-time checked `X-Admin-Token`; read-only dashboard APIs are
  unchanged.

## Phase 8C Verification

- Full server suite: `127 passed`, including new slow-client, concurrent-publish,
  maintenance-auth, activity-ordering, migration, and pipeline tests.
- Slow-client fault test confirms the fast socket receives within 30 ms while the
  blocked socket times out and is removed; concurrent sends reach a maximum depth of
  one per socket.
- `compileall` for `server/apps` and `server/core`: PASS.
- `node --check` for modified `ws.js`, `api.js`, `nodes.js`, and `ui.js`: PASS.
- `git diff --check`: PASS with Windows LF-to-CRLF warnings only.

## Phase 8D Final Regression And Hardware Checklist

- Final production firmware builds after fallback channel sweep:
  - `node_imu`: PASS, RAM 57,948 bytes, flash 779,597 bytes.
  - `node_ppg`: PASS, RAM 57,256 bytes, flash 780,649 bytes.
  - `node_gateway`: PASS, RAM 46,932 bytes, flash 796,381 bytes.
- Final server suite remains `127 passed`; Python compileall and modified JavaScript
  syntax checks pass.
- Model discovery smoke test launched from `C:\tmp`: PASS, absolute model directory
  resolved correctly and both models loaded (`2 registered`, `2 active`).
- Only `COM4` and `COM5` are present and registry mapping identifies both as
  Bluetooth modem ports (`BthModem0/1`), not USB ESP32 devices. No firmware was
  uploaded and no physical-radio result is claimed.

Hardware acceptance steps when three ESP32 USB ports are available:

1. Record each STA MAC and assign gateway, IMU, and PPG roles; verify the IMU MAC is
   `68:09:47:77:19:04` or update `hardware.h` before flashing.
2. Flash all three production environments and capture all serial logs for at least
   60 seconds. Require PPG-to-IMU RSSI reports to show `ok=Y` and RSSI values other
   than the unknown `-127` sentinel after gateway beacons begin.
3. Confirm both `cs_imu` and `cs_ppg` reach MQTT and produce monotonically ordered
   group windows, with no sustained `PAIR_MISMATCH`, overflow, or processor errors.
4. Force direct-link degradation to exercise relay routing, then restore signal;
   verify route changes and gateway source/relay identity checks without packet loss.
5. Start a sensor with invalid Wi-Fi credentials while the gateway uses a channel
   other than 1; require fallback sweep logs followed by channel confirmation and
   successful ESP-NOW delivery.
6. Restart the AP on a different channel and verify gateway reconnect, peer channel
   resynchronization, and resumed sensor traffic without power-cycling sensors.
7. Start the server with MQTT absent, then start the broker; stop/restart it again
   during traffic and verify ingestion resumes, retained Last Will reports gateway
   offline, and no worker thread dies.
8. Repeat Wi-Fi outages twice around the configured restart threshold to verify the
   outage timer resets after recovery and does not cause a premature second restart.

Automated audit/fix scope is complete. Physical RSSI, RF loss, channel migration,
and three-device timing remain acceptance tests rather than unresolved code claims.

## Phase 2 Verified Contracts

- Firmware and server currently select the same 32 Hadamard rows for seed zero,
  with identical signs and maximum float32-versus-float64 element difference
  `3.781755325560354e-10`.
- Current packed sizes from the declarations are `CS1AxisPacket=140`,
  `CSPpgPacket=146`, and routed maxima 148/154 bytes including the 8-byte wrapper;
  all remain below the 250-byte ESP-NOW payload limit.
- Historical end-to-end captures show normal IMU MQTT JSON around 1514-1516 bytes
  and PPG JSON around 310-311 bytes, within the current 2048-byte message and
  2200-byte PubSubClient buffers.

## Phase 2 Build And Test Verification

- Server `test_cs_hadamard.py` plus `test_validator.py`: `38 passed`.
- Deterministic firmware-algorithm replica versus server Phi: PASS.
- `node_gateway`: PASS, RAM 46,932 bytes, flash 793,925 bytes.
- `test_cs_compression`: PASS, RAM 34,152 bytes, flash 275,189 bytes.
- `tools.verify_phi`: FAIL as documented in PACKET-004.
- No USB ESP32 was available to execute the compiled CS sketch or packet timing
  checks on hardware.

## Phase 3 Verified Runtime Path

- `sendRssiReport()` chooses the opposite sensor's compile-time MAC and returns
  `_send()` directly; the end-to-end test prints that exact boolean as `ok`.
- The 2026-07-17 capture records all three devices on channel 6. IMU and PPG data
  both reached the gateway, so the repeated PPG RSSI-report failure is not a
  general PPG radio or channel failure.
- In that capture, actual PPG and gateway STA MACs match the constants, while the
  actual IMU STA MAC does not. This isolates `CONFIG-001` as the direct cause of
  the recorded PPG-to-IMU `ok=0` pattern.
- `-127` has a separate direct cause: `_lastBeaconRssi` starts at the unknown
  sentinel and can only be updated by a promiscuous callback that currently
  rejects ESP-NOW management/action frames as non-data.
- ESP-IDF 4.4 documentation confirms callback execution in the Wi-Fi task and
  recommends waiting for each send callback before the next send. It also lists
  absent destinations and channel mismatch among normal MAC-layer send failures.

## Phase 3 Build And Test Verification

- Production `node_imu`, `node_ppg`, and `node_gateway`: PASS.
- Automated mesh environments for gateway, node 1, and node 2: PASS build.
- End-to-end environments for gateway, node 1, and node 2: PASS build.
- Build matrix total: `9 succeeded` in 110.435 seconds; no compiler errors.
- Mesh test sketches compile but require three connected ESP32 devices to execute
  their radio assertions. No USB ESP32 serial port was available during this
  phase, so corrected RSSI and recovery behavior remain hardware-verification
  items for Phase 8.

## Phase 4 Verified Routing Path

- Production flow is source `DynamicRouter::decide()` -> hard-coded peer MAC ->
  two first-hop attempts -> relay `taskSensorReceiver` -> one wrapped gateway send
  -> gateway unwrap/IMU accumulation -> volatile MQTT queue -> PubSubClient.
- Archived QoS testing exercised 64 direct and 68 relayed transmissions with no
  reported packet loss under scripted RSSI and stable channels. This confirms the
  nominal relay format works, but it does not exercise real beacon RSSI, weak
  source-relay links, Wi-Fi channel changes, MQTT failure, or reboot recovery.
- The archived routing runs contain three transition samples where the expected
  route was DIRECT but the actual route remained RELAY. They are consistent with
  asynchronously updated RSSI state and the absence of route hysteresis.
- Gateway accepts packet-declared identities for direct and relayed traffic; no
  reviewed routing path maps `RawPacket::srcMac` back to the configured node role.
- Gateway Wi-Fi reconnect and ESP-NOW channel/peer synchronization are independent
  subsystems after initial setup, leaving no recovery contract between them.

## Phase 4 Build And Test Verification

- Production firmware build results from Phase 3 remain PASS for IMU, PPG, and
  gateway; Phase 4 introduced no production source changes.
- `test_mesh_gateway`: PASS, RAM 45,192 bytes, flash 746,505 bytes.
- `test_mesh_sensor_n1`: FAIL compile at `manual_mesh_routing.cpp:132` because the
  sketch calls the obsolete eight-argument `sendCsPpg()` signature.
- `test_mesh_sensor_n2`: FAIL compile for the same obsolete signature.
- `test_mesh_routing.cpp` remains stale and is not wired to a current dedicated
  PlatformIO test environment; several integration cases are unconditional
  `TEST_PASS()` placeholders.
- Hardware fault injection for relay first-hop loss, relay-to-gateway loss, MQTT
  failure, repeated Wi-Fi outage, and channel-changing reconnect remains required
  in Phase 7/8 because no USB ESP32 serial devices were available.

## Phase 5 Verified Server Path

- Runtime flow is MQTT callback -> topic/group lookup -> shared validator -> one-slot
  `NodeState` modality buffers -> global two-worker executor -> seven OMP
  reconstructions -> quality assessment -> SQLite -> ML -> WebSocket notifier.
- Group 1 currently maps physical IMU node 1 and PPG node 2. Their firmware clocks
  are independent, yet server output uses the IMU timestamp and accepts every
  numeric cross-node spread after warning.
- Controlled experiments confirmed missing CS means pass validation, invalid data
  can advance timestamp state, a newer modality overwrites an unmatched older one,
  a 7000 ms mismatched pair dispatches, NaN/Infinity timestamps raise, and two
  same-group windows can complete in reverse order.
- No listener or `NodeState` unit tests exist. The sole full-pipeline test does not
  reach processing because it constructs the old `NodeState(node_id=...)` API.
- Nominal Hadamard/OMP implementation and valid vector validation remain covered by
  passing tests; these results do not cover pairing, lifecycle, or failure recovery.

## Phase 5 Build And Test Verification

- `pytest tests/test_validator.py tests/test_cs_hadamard.py`: `38 passed`.
- `test_full_pipeline.py`: FAIL before pipeline execution because of obsolete
  `NodeState.__init__(node_id=...)`, matching BASE-001.
- Combined targeted run: `38 passed, 1 failed, 1 warning`.
- `compileall` for `apps/reconstruct`, `core/validator.py`, and `cs`: PASS.
- MQTT broker restart, malformed-message worker survival, QoS interruption, and
  sustained processor-overload tests remain Phase 7 integration work.

## Phase Exit Checklist

A phase is complete only when:

- every scoped source file and runtime boundary has been reviewed;
- findings have evidence and severity;
- relevant production builds or automated tests have run;
- hardware-only gaps are explicitly marked;
- no unrelated source changes were introduced;
- the plan and this ledger have been updated.

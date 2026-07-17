import time
import json
import random
import paho.mqtt.client as mqtt

import numpy as np

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
TOPIC = "health_monitor/node_1/combined"
# =============================================================================
# Self-contained Hadamard Φ Matrix Generation (to avoid circular imports)
# =============================================================================
def _build_hadamard(n: int) -> np.ndarray:
    if n == 1:
        return np.array([[1.0]])
    h = np.array([[1.0]])
    while h.shape[0] < n:
        h = np.block([[h, h], [h, -h]])
    return h

def _lcg_generator(seed: int):
    state = seed & 0xFFFFFFFF
    while True:
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        yield state

def generate_phi(seed: int, m: int, n: int) -> np.ndarray:
    H   = _build_hadamard(n)
    gen = _lcg_generator(seed)
    d = np.array(
        [(1 if (next(gen) & 1) else -1) for _ in range(n)],
        dtype=np.float64
    )
    idx = list(range(n))
    for i in range(m):
        r = next(gen)
        j = i + int(r % (n - i))
        idx[i], idx[j] = idx[j], idx[i]
    row_idx = sorted(idx[:m])
    scale = 1.0 / np.sqrt(float(n) * float(m))
    phi   = np.zeros((m, n), dtype=np.float64)
    for mi, row in enumerate(row_idx):
        phi[mi, :] = d[row] * scale * H[row, :]
    return phi

# Generate Φ (32 × 64) deterministically matching the server and firmware
PHI = generate_phi(seed=0, m=32, n=64)

MQTT_BROKER = "localhost"
MQTT_PORT = 1883

client = mqtt.Client(client_id="dummy_sender")
client.connect(MQTT_BROKER, MQTT_PORT, 60)
client.loop_start()

print(f"Connected to MQTT Broker at {MQTT_BROKER}:{MQTT_PORT}")
print(f"Publishing dummy CS projected packets...")

start_time = time.time()

def compress_signal(raw_signal):
    x = np.array(raw_signal, dtype=float)
    mean_val = np.mean(x)
    x_centered = x - mean_val
    y = PHI.dot(x_centered)  # Shape (32,)
    return y.tolist(), float(mean_val)

try:
    while True:
        ts = int(time.time() * 1000)
        
        # State machine cycling every 10 seconds
        elapsed_cycles = int((time.time() - start_time) / 10) % 4
        
        if elapsed_cycles == 0:
            # STATE 0: DUDUK (Sitting)
            # Low acceleration (~1.0 G on Z, ~0 on X/Y), very low gyro
            ax_raw = [random.uniform(-0.05, 0.05) * 9.80665 for _ in range(64)]
            ay_raw = [random.uniform(-0.05, 0.05) * 9.80665 for _ in range(64)]
            az_raw = [(1.0 + random.uniform(-0.02, 0.02)) * 9.80665 for _ in range(64)]
            gx_raw = [random.uniform(-5, 5) for _ in range(64)]
            gy_raw = [random.uniform(-5, 5) for _ in range(64)]
            gz_raw = [random.uniform(-5, 5) for _ in range(64)]
            state_name = "DUDUK"
        elif elapsed_cycles == 1:
            # STATE 1: JALAN (Walking)
            # Oscillation on X/Y/Z, medium gyro
            t_vec = np.linspace(0, 4*np.pi, 64)
            ax_raw = (np.sin(t_vec) * 0.2 * 9.80665 + random.uniform(-0.02, 0.02)).tolist()
            ay_raw = (np.cos(t_vec) * 0.2 * 9.80665 + random.uniform(-0.02, 0.02)).tolist()
            az_raw = (1.0 + np.sin(2*t_vec) * 0.3 * 9.80665).tolist()
            gx_raw = (np.sin(t_vec) * 40).tolist()
            gy_raw = (np.cos(t_vec) * 40).tolist()
            gz_raw = (np.sin(2*t_vec) * 20).tolist()
            state_name = "JALAN"
        elif elapsed_cycles == 2:
            # STATE 2: JATUH (Fall)
            # Huge spike in acceleration (impact), then zero/low (stillness)
            ax_raw = [random.uniform(-0.05, 0.05) * 9.80665 for _ in range(64)]
            ay_raw = [random.uniform(-0.05, 0.05) * 9.80665 for _ in range(64)]
            az_raw = [(1.0 + random.uniform(-0.05, 0.05)) * 9.80665 for _ in range(64)]
            gx_raw = [random.uniform(-5, 5) for _ in range(64)]
            gy_raw = [random.uniform(-5, 5) for _ in range(64)]
            gz_raw = [random.uniform(-5, 5) for _ in range(64)]
            for i in range(25, 35):
                ax_raw[i] = random.uniform(3.0, 4.5) * 9.80665
                ay_raw[i] = random.uniform(3.0, 4.5) * 9.80665
                az_raw[i] = random.uniform(-0.2, 0.2) * 9.80665
                gx_raw[i] = random.uniform(250, 450)
                gy_raw[i] = random.uniform(250, 450)
                gz_raw[i] = random.uniform(250, 450)
            state_name = "JATUH"
        else:
            # STATE 3: TIDUR (Sleeping / Still flat)
            # Low acceleration on side (e.g. 1.0 G on X, ~0 on Y/Z), zero gyro
            ax_raw = [(1.0 + random.uniform(-0.01, 0.01)) * 9.80665 for _ in range(64)]
            ay_raw = [random.uniform(-0.01, 0.01) * 9.80665 for _ in range(64)]
            az_raw = [random.uniform(-0.01, 0.01) * 9.80665 for _ in range(64)]
            gx_raw = [random.uniform(-1, 1) for _ in range(64)]
            gy_raw = [random.uniform(-1, 1) for _ in range(64)]
            gz_raw = [random.uniform(-1, 1) for _ in range(64)]
            state_name = "TIDUR"
        
        # Compress signals
        ax_y, ax_mean = compress_signal(ax_raw)
        ay_y, ay_mean = compress_signal(ay_raw)
        az_y, az_mean = compress_signal(az_raw)
        gx_y, gx_mean = compress_signal(gx_raw)
        gy_y, gy_mean = compress_signal(gy_raw)
        gz_y, gz_mean = compress_signal(gz_raw)
        
        ir_raw = [random.uniform(100000, 150000) for _ in range(64)]
        ir_y, ir_mean = compress_signal(ir_raw)
        
        hr = random.randint(110, 130) if state_name != "TIDUR" else random.randint(60, 75)
        spo2 = random.uniform(96.0, 99.0) if state_name != "TIDUR" else random.uniform(98.0, 99.5)
        
        payload_imu = {
            "ts": ts,
            "ax": ax_y, "ay": ay_y, "az": az_y,
            "gx": gx_y, "gy": gy_y, "gz": gz_y,
            "mean_ax": ax_mean, "mean_ay": ay_mean, "mean_az": az_mean,
            "mean_gx": gx_mean, "mean_gy": gy_mean, "mean_gz": gz_mean,
            "finger": True
        }
        
        payload_ppg = {
            "ts": ts,
            "ir": ir_y,
            "mean_ir": ir_mean,
            "hr": hr, "spo2": spo2,
            "ppg_valid": True,
            "finger": True
        }
        
        client.publish("health_monitor/node_1/cs_imu", json.dumps(payload_imu))
        client.publish("health_monitor/node_2/cs_ppg", json.dumps(payload_ppg))
        
        print(f"[Sent] Profile={state_name} at ts={ts}")
        
        time.sleep(1) # Kirim setiap 1 detik


except KeyboardInterrupt:
    print("\nStopping dummy sender...")
    client.loop_stop()
    client.disconnect()

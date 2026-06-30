import time
import json
import random
import paho.mqtt.client as mqtt

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
TOPIC = "health_monitor/node_1/combined"

client = mqtt.Client(client_id="dummy_sender")
client.connect(MQTT_BROKER, MQTT_PORT, 60)
client.loop_start()

print(f"Connected to MQTT Broker at {MQTT_BROKER}:{MQTT_PORT}")
print(f"Publishing dummy combined packet to {TOPIC}...")

try:
    while True:
        ts = int(time.time() * 1000)
        
        # Array 32 elemen untuk compressive sensing (diharapkan oleh server)
        # Kita beri nilai ekstrem agar model SVM mungkin memprediksi "jatuh"
        ax_arr = [random.uniform(5.0, 15.0) for _ in range(32)]
        ay_arr = [random.uniform(5.0, 15.0) for _ in range(32)]
        az_arr = [random.uniform(5.0, 15.0) for _ in range(32)]
        gx_arr = [random.uniform(100.0, 300.0) for _ in range(32)]
        gy_arr = [random.uniform(100.0, 300.0) for _ in range(32)]
        gz_arr = [random.uniform(100.0, 300.0) for _ in range(32)]
        
        ir_arr = [random.uniform(100000, 150000) for _ in range(32)]
        hr = random.randint(110, 130)
        spo2 = random.uniform(96.0, 99.0)
        
        payload_imu = {
            "ts": ts,
            "ax": ax_arr, "ay": ay_arr, "az": az_arr,
            "gx": gx_arr, "gy": gy_arr, "gz": gz_arr,
            "mean_ax": ax_arr[0], "mean_ay": ay_arr[0], "mean_az": az_arr[0],
            "mean_gx": gx_arr[0], "mean_gy": gy_arr[0], "mean_gz": gz_arr[0],
            "finger": True
        }
        
        payload_ppg = {
            "ts": ts,
            "ir": ir_arr,
            "mean_ir": ir_arr[0],
            "hr": hr, "spo2": spo2,
            "ppg_valid": True,
            "finger": True
        }
        
        client.publish("health_monitor/node_1/cs_imu", json.dumps(payload_imu))
        # PPG dikirim ke node_2 karena server menggunakan NODE_GROUPS (imu=1, ppg=2)
        client.publish("health_monitor/node_2/cs_ppg", json.dumps(payload_ppg))
        
        print(f"[Sent] cs_imu(node_1) and cs_ppg(node_2) at ts={ts}")
        
        time.sleep(1) # Kirim setiap 1 detik

except KeyboardInterrupt:
    print("\nStopping dummy sender...")
    client.loop_stop()
    client.disconnect()

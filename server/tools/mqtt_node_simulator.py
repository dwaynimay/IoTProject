"""
mqtt_node_simulator.py — Simulasi node ESP32 yang publish MQTT messages.

Gunakan untuk testing server tanpa hardware ESP32.
"""

import json
import time
import random
import logging
from typing import Optional

import paho.mqtt.client as mqtt

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='[%(asctime)s] [%(levelname)s] %(message)s'
)
logger = logging.getLogger(__name__)


class NodeSimulator:
    """Simulasi ESP32 node yang publish ke MQTT."""

    def __init__(
        self,
        node_id: int,
        broker: str = "localhost",
        port: int = 1883,
        topic_base: str = "health_monitor",
        cs_m: int = 32,  # Jumlah compressed samples
        stress_mode: bool = False,  # Simulasi kondisi stress
    ):
        self.node_id = node_id
        self.broker = broker
        self.port = port
        self.topic_base = topic_base
        self.cs_m = cs_m
        self.stress_mode = stress_mode  # True = tachycardia, high movement
        
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_publish = self._on_publish
        
        self.connected = False
        self.window_count = 0
        self.ts_base = int(time.time() * 1000)  # timestamp mulai (ms)

    def connect(self) -> bool:
        """Connect ke MQTT broker."""
        try:
            logger.info(f"Node {self.node_id}: Connecting to {self.broker}:{self.port}...")
            self.client.connect(self.broker, self.port, keepalive=60)
            self.client.loop_start()
            
            # Tunggu sampai connected (max 5 detik)
            for _ in range(50):
                if self.connected:
                    logger.info(f"Node {self.node_id}: Connected!")
                    return True
                time.sleep(0.1)
            
            logger.error(f"Node {self.node_id}: Connection timeout")
            return False
        except Exception as e:
            logger.error(f"Node {self.node_id}: Connection failed: {e}")
            return False

    def disconnect(self) -> None:
        """Disconnect dari broker."""
        self.client.loop_stop()
        self.client.disconnect()

    def _on_connect(self, client, userdata, connect_flags, reason_code, properties=None):
        if reason_code.is_failure:
            logger.error(f"Node {self.node_id}: Failed to connect: {reason_code}")
            self.connected = False
        else:
            logger.info(f"Node {self.node_id}: Connected successfully")
            self.connected = True

    def _on_disconnect(self, client, userdata, disconnect_flags, reason_code, properties=None):
        logger.warning(f"Node {self.node_id}: Disconnected")
        self.connected = False

    def _on_publish(self, client, userdata, mid, reason_code=None, properties=None):
        pass  # Silent

    def _generate_imu_signal(self) -> dict:
        """Generate simulated IMU signal (cs_imu)."""
        # Simulasi dengan noise kecil
        if self.stress_mode:
            # Stress mode: gerakan lebih agresif, magnitude lebih besar
            ax = [0.1 + random.uniform(-0.05, 0.05) for _ in range(self.cs_m)]
            ay = [0.15 + random.uniform(-0.08, 0.08) for _ in range(self.cs_m)]
            az = [0.2 + random.uniform(-0.05, 0.05) for _ in range(self.cs_m)]
            gx = [random.uniform(-0.3, 0.3) for _ in range(self.cs_m)]
            gy = [random.uniform(-0.3, 0.3) for _ in range(self.cs_m)]
            gz = [random.uniform(-0.3, 0.3) for _ in range(self.cs_m)]
            logger.warning(f"Node {self.node_id}: STRESS MODE - High acceleration/gyro")
        else:
            # Normal mode: gerakan ringan
            ax = [0.01 + random.uniform(-0.005, 0.005) for _ in range(self.cs_m)]
            ay = [0.02 + random.uniform(-0.005, 0.005) for _ in range(self.cs_m)]
            az = [0.1 + random.uniform(-0.01, 0.01) for _ in range(self.cs_m)]
            gx = [random.uniform(-0.1, 0.1) for _ in range(self.cs_m)]
            gy = [random.uniform(-0.1, 0.1) for _ in range(self.cs_m)]
            gz = [random.uniform(-0.1, 0.1) for _ in range(self.cs_m)]
        
        return {
            "ts": self.ts_base + self.window_count * 1000,
            "finger": random.choice([True, True, True, False]),  # 75% ada jari
            "ax": ax,
            "ay": ay,
            "az": az,
            "gx": gx,
            "gy": gy,
            "gz": gz,
        }

    def _generate_ppg_signal(self) -> dict:
        """Generate simulated PPG signal (cs_ppg)."""
        # Simulasi heartbeat pattern
        base_ir = 100.0
        ir = [base_ir + 5 * (i % 10) / 10.0 + random.uniform(-0.5, 0.5) 
              for i in range(self.cs_m)]
        
        if self.stress_mode:
            # Stress mode: HR tinggi (tachycardia), variasi PPG lebih besar
            hr = random.randint(120, 150)  # Tachycardia: >100 bpm
            # PPG pattern lebih aggresive (high amplitude)
            ir = [base_ir + 15 * (i % 10) / 10.0 + random.uniform(-2.0, 2.0) 
                  for i in range(self.cs_m)]
            logger.warning(f"Node {self.node_id}: STRESS MODE - Tachycardia HR={hr} bpm")
        else:
            # Normal mode
            hr = random.randint(60, 100)  # Normal: 60-100 bpm
        
        return {
            "ts": self.ts_base + self.window_count * 1000,
            "hr": hr,
            "finger": random.choice([True, True, True, False]),  # 75% ada jari
            "ir": ir,
        }

    def publish_imu(self) -> bool:
        """Publish cs_imu message."""
        try:
            payload = self._generate_imu_signal()
            topic = f"{self.topic_base}/node_{self.node_id}/cs_imu"
            
            result = self.client.publish(
                topic,
                json.dumps(payload),
                qos=1
            )
            
            if result.rc == mqtt.MQTT_ERR_SUCCESS:
                logger.info(f"Node {self.node_id}: Published cs_imu (window #{self.window_count})")
                return True
            else:
                logger.error(f"Node {self.node_id}: Publish failed: {result.rc}")
                return False
        except Exception as e:
            logger.error(f"Node {self.node_id}: Publish error: {e}")
            return False

    def publish_ppg(self) -> bool:
        """Publish cs_ppg message."""
        try:
            payload = self._generate_ppg_signal()
            topic = f"{self.topic_base}/node_{self.node_id}/cs_ppg"
            
            result = self.client.publish(
                topic,
                json.dumps(payload),
                qos=1
            )
            
            if result.rc == mqtt.MQTT_ERR_SUCCESS:
                logger.info(f"Node {self.node_id}: Published cs_ppg (window #{self.window_count})")
                return True
            else:
                logger.error(f"Node {self.node_id}: Publish failed: {result.rc}")
                return False
        except Exception as e:
            logger.error(f"Node {self.node_id}: Publish error: {e}")
            return False

    def publish_window(self) -> bool:
        """Publish satu window (cs_imu + cs_ppg)."""
        if not self.connected:
            logger.warning(f"Node {self.node_id}: Not connected, skipping publish")
            return False
        
        self.window_count += 1
        success = True
        
        # Publish IMU
        if not self.publish_imu():
            success = False
        
        time.sleep(0.1)  # Delay kecil antara IMU dan PPG
        
        # Publish PPG
        if not self.publish_ppg():
            success = False
        
        return success

    def simulate_continuous(self, interval_sec: float = 1.0, count: Optional[int] = None):
        """
        Simulasi continuous publish.
        
        Args:
            interval_sec: Interval antara window publish (detik)
            count: Jumlah window yang di-publish. None = infinite
        """
        logger.info(f"Node {self.node_id}: Starting continuous simulation (interval={interval_sec}s, count={count})")
        
        window = 0
        try:
            while count is None or window < count:
                if self.publish_window():
                    window += 1
                time.sleep(interval_sec)
        except KeyboardInterrupt:
            logger.info(f"Node {self.node_id}: Simulation stopped by user")


def main():
    """Main function untuk testing."""
    import argparse
    
    parser = argparse.ArgumentParser(description="Simulasi MQTT node ESP32")
    parser.add_argument("--broker", default="localhost", help="MQTT broker host")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port")
    parser.add_argument("--node-id", type=int, default=1, help="Node ID")
    parser.add_argument("--topic-base", default="health_monitor", help="MQTT topic prefix")
    parser.add_argument("--interval", type=float, default=1.0, help="Publish interval (detik)")
    parser.add_argument("--count", type=int, default=None, help="Jumlah window (None = infinite)")
    parser.add_argument("--cs-m", type=int, default=32, help="Compressed samples")
    parser.add_argument("--stress", action="store_true", help="Enable stress mode (tachycardia + high motion)")
    
    args = parser.parse_args()
    
    # Create node simulator
    node = NodeSimulator(
        node_id=args.node_id,
        broker=args.broker,
        port=args.port,
        topic_base=args.topic_base,
        cs_m=args.cs_m,
        stress_mode=args.stress,
    )
    
    # Connect dan simulate
    if node.connect():
        try:
            node.simulate_continuous(
                interval_sec=args.interval,
                count=args.count
            )
        finally:
            node.disconnect()
    else:
        logger.error("Failed to connect to broker")


if __name__ == "__main__":
    main()

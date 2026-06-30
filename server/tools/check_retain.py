import paho.mqtt.client as mqtt
import time

def on_message(client, userdata, message):
    print(f"Received: {message.topic} - Retained: {message.retain} - Payload: {len(message.payload)} bytes")

client = mqtt.Client()
client.on_message = on_message
client.connect("localhost", 1883)
client.subscribe("health_monitor/#")

client.loop_start()
time.sleep(2)
client.loop_stop()
client.disconnect()

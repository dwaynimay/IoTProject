import paho.mqtt.client as mqtt
import time
import sys

def on_message(client, userdata, message):
    print(f"Received: {message.topic} - Payload size: {len(message.payload)} bytes")
    # Do not disconnect, keep listening

client = mqtt.Client()
client.on_message = on_message
client.connect("localhost", 1883)
client.subscribe("health_monitor/#")

print("Listening for 10 seconds...")
client.loop_start()
time.sleep(10)
client.loop_stop()
client.disconnect()
print("Done")

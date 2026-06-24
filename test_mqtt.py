import paho.mqtt.client as mqtt
import time

def on_message(client, userdata, message):
    print("Received:", message.topic)
    client.disconnect()

client = mqtt.Client()
client.on_message = on_message
client.connect("localhost", 1883)
client.subscribe("#")
client.loop_forever()

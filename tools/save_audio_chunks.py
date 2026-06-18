"""
Subscribes to the ESP32 snoring detector MQTT topic and saves each received
audio chunk as a WAV file. Filenames are the chunk timestamp in microseconds
since the ESP32 booted (from esp_timer_get_time()).

Message layout (little-endian):
  [0:8]   int64   timestamp_us
  [8:]    int16[] PCM samples at 16 kHz, mono

Usage:
    pip install paho-mqtt
    python save_audio_chunks.py [--broker <host>] [--port <port>] [--out <dir>]
"""

import argparse
import os
import struct
import wave

import paho.mqtt.client as mqtt

TOPIC       = "audio/raw"
SAMPLE_RATE = 16000
CHANNELS    = 1
SAMPLE_WIDTH = 2  # bytes per sample (int16)
NUM_SAMPLES  = 32000

TIMESTAMP_FMT = "<q"   # little-endian int64
TIMESTAMP_SIZE = struct.calcsize(TIMESTAMP_FMT)


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("Connected to broker")
        client.subscribe(TOPIC)
        print(f"Subscribed to '{TOPIC}', saving chunks to '{userdata['out_dir']}'")
    else:
        print(f"Connection failed (rc={rc})")


def on_message(client, userdata, message):
    payload = message.payload
    expected = TIMESTAMP_SIZE + NUM_SAMPLES * SAMPLE_WIDTH
    if len(payload) != expected:
        print(f"Unexpected payload size {len(payload)} (expected {expected}), skipping")
        return

    timestamp_us = struct.unpack_from(TIMESTAMP_FMT, payload, 0)[0]
    pcm_bytes    = payload[TIMESTAMP_SIZE:]

    out_path = os.path.join(userdata["out_dir"], f"{timestamp_us}.wav")
    with wave.open(out_path, "wb") as f:
        f.setnchannels(CHANNELS)
        f.setsampwidth(SAMPLE_WIDTH)
        f.setframerate(SAMPLE_RATE)
        f.writeframes(pcm_bytes)

    duration_s = NUM_SAMPLES / SAMPLE_RATE
    print(f"Saved {out_path}  ({duration_s:.1f} s, ts={timestamp_us} µs since boot)")


def main():
    parser = argparse.ArgumentParser(description="Save ESP32 audio chunks as WAV files")
    parser.add_argument("--broker", default="50.116.52.213", help="MQTT broker host")
    parser.add_argument("--port",   default=2974, type=int,   help="MQTT broker port")
    parser.add_argument("--out",    default="audio_chunks",   help="Output directory")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)

    client = mqtt.Client(userdata={"out_dir": args.out})
    client.on_connect = on_connect
    client.on_message = on_message

    print(f"Connecting to {args.broker}:{args.port} ...")
    client.connect(args.broker, args.port)
    client.loop_forever()


if __name__ == "__main__":
    main()

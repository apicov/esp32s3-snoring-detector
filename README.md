# ESP32-S3 Camera MQTT

ESP32-S3 camera application with MQTT support for TinyML image processing. Captures images, processes them using PSRAM, and sends 96x96 RGB888 images via MQTT for machine learning inference.

## Features

- Camera capture with OV2640 sensor (JPEG format)
- JPEG to RGB888 conversion and resizing (160x120 to 96x96)
- PSRAM utilization for image processing buffers
- MQTT command-driven snapshot capture
- Base64 encoding for image transmission
- Dual-core operation for better performance
- WiFi connectivity

## Hardware Requirements

- ESP32-S3 with 8MB PSRAM (e.g., XIAO ESP32S3 Sense)
- OV2640 camera module
- WiFi network access
- MQTT broker

## Configuration

Run `idf.py menuconfig` and configure:

1. **Camera MQTT Configuration**
   - WiFi SSID
   - WiFi Password
   - MQTT Broker URI
   - MQTT Image Topic (default: `/camera/img`)
   - MQTT Command Topic (default: `/camera/cmd`)

## Building and Flashing

```bash
idf.py build
idf.py flash monitor
```

## Usage

Send a "snap" command to the MQTT command topic to trigger a snapshot:

```bash
mosquitto_pub -h <broker-ip> -p <port> -t /camera/cmd -m "snap"
```

The camera will capture an image, process it to 96x96 RGB888 format, base64 encode it, and publish to the image topic.

## Image Processing Pipeline

1. Capture JPEG image (160x120, ~2KB)
2. Decode JPEG to RGB888 format (160x120)
3. Resize to 96x96 for TinyML models
4. Base64 encode the RGB data
5. Publish via MQTT

All processing buffers are allocated in PSRAM to preserve internal RAM for system operations.

## PSRAM Configuration

The project uses `CONFIG_SPIRAM_USE_CAPS_ALLOC=y` configuration, which means:
- PSRAM is only used for explicitly allocated buffers via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
- WiFi, lwIP, and system libraries use internal RAM
- Avoids heap corruption issues in unicore mode

## Technical Details

- ESP-IDF v6.0.1
- Dual-core mode enabled
- PSRAM: 8MB Octal mode at 80MHz
- Camera: QQVGA (160x120) JPEG capture
- Output: 96x96 RGB888 (base64 encoded, ~36KB)

## Components

- `camera_ctl`: Camera initialization and control
- `mqtt_client`: MQTT client wrapper using lwmqtt
- `wifi_station`: WiFi station management
- `image_utils`: Image processing utilities (resize, color conversion)

## License

See LICENSE file for details.

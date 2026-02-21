# ESP32-S3 N16R8 + Waveshare 2.4" ILI9341 Web 3D Viewer

This project turns your ESP32-S3 into:

- A Wi-Fi Access Point (AP)
- A local control website
- A simple wireframe 3D engine for `.obj` files
- A text overlay system for your LCD

You can upload `.obj` files from the website, then choose which model and text to show on the display.

---

## Features

- **AP mode only** (no router required)
- **Web UI** at `http://192.168.4.1`
- **OBJ support** (subset):
  - `v x y z`
  - `f ...` polygons (triangles/quads/ngons supported as wireframe edges)
- **Live controls**:
  - Pick active model
  - Set overlay text
  - Change model scale
  - Upload new `.obj`

---

## Hardware wiring (example)

> Pin mapping depends on your exact board + display wiring.  
> If your screen does not work, adjust pin constants at top of `src/main.cpp`.

| LCD Pin | ESP32-S3 Pin (default in code) |
|---|---|
| CS | GPIO10 |
| DC | GPIO9 |
| RST | GPIO14 |
| SCK | GPIO12 |
| MOSI | GPIO11 |
| MISO | GPIO13 |
| BL | GPIO48 (optional) |
| VCC | 3V3 |
| GND | GND |

---

## Build & flash (PlatformIO)

1. Install [PlatformIO](https://platformio.org/).
2. Connect your ESP32-S3 board by USB.
3. Flash firmware:

```bash
pio run -e esp32s3 -t upload
```

4. Upload SPIFFS web/model files:

```bash
pio run -e esp32s3 -t uploadfs
```

5. Open serial monitor:

```bash
pio device monitor -b 115200
```

---

## First use

1. Connect phone/PC to Wi-Fi:
   - SSID: `ESP32S3-3D-Viewer`
   - Password: `12345678`
2. Open browser: `http://192.168.4.1`
3. Choose model + text, click **Apply to LCD**.

---

## Adding your own OBJ models

### Option A: Upload from website
- Use **Upload new OBJ** in the web page.

### Option B: Copy before flashing
- Put files in `data/models/*.obj`
- Run `pio run -e esp32s3 -t uploadfs` again.

### Recommended model complexity

For smooth rendering on ESP32:

- Vertices: up to ~3,500
- Edges: up to ~12,000

Large/complex models will render slower.

---

## Notes

- Renderer is wireframe (fast and simple for microcontrollers).
- The engine rotates the model continuously.
- If your Waveshare panel is a different ILI variant, update the display library/pins accordingly.

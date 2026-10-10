# Bluetooth Classic HID Host

Native ESP-IDF 6.1 application for the original ESP32. It continuously scans for Bluetooth Classic peripherals, attempts a HID connection, requests boot protocol, and logs raw reports plus decoded standard keyboard and mouse input.

On the target mouse, report ID `0x03` carries left/right/bottom button masks in byte 1 (`0x10`, `0x20`, and `0x04`). A left or right release starts that strip blinking yellow; releasing the bottom button cancels both patterns. Blink timing is 300 ms per phase.

## Hardware and configuration

- Use an original ESP32 with Bluetooth Classic support, such as ESP32 Dev Module. ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, and ESP32-H2 do not support Bluetooth Classic.
- The project enables Bluedroid Classic Bluetooth and the HID Host profile in `sdkconfig.defaults`.
- A candidate is selected by its Bluetooth Class of Device peripheral category. HID connection then confirms whether it provides the Classic HID service.
- Two WS2812/NeoPixel strips are configured for 8 pixels each on GPIO 18 (left) and GPIO 19 (right). Change `LEFT_STRIP_GPIO`, `RIGHT_STRIP_GPIO`, and `STRIP_PIXEL_COUNT` in `main/main.c` to match your wiring.
- Use an external 5 V supply for the strips as needed, and connect its ground to ESP32 ground. Do not power a large strip from an ESP32 GPIO or 3.3 V pin.

## Build and flash

Open an ESP-IDF 6.1 terminal in this project directory and run:

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the ESP32 serial port. Pairing confirmation is accepted automatically; legacy PIN requests are answered with `0000`. Put the keyboard or mouse into pairing mode before starting the monitor.

Standard boot keyboard reports produce key down/up events. Standard boot mouse reports produce button, relative X/Y, and wheel values. Raw report bytes are also logged. Devices that do not support boot protocol may still provide raw input reports, but this application does not decode their device-specific report layouts.
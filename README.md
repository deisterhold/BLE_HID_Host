# ESPHome Classic HID component

This external component owns the ESP32 Bluedroid Classic HID host and emits generic events. It does not name mouse buttons or interpret report layouts: `on_input` receives the device address, protocol mode (`0` boot or `1` report), and a copy of the raw HID input bytes. ESPHome lambdas can filter by address, report ID, payload, and button bits.

`classic_bt_hid_example.yaml` configures the two SK6812 RGBW strips in YAML and shows the observed mouse's release filters (`report ID 0x03`, button bits `0x10`, `0x20`, and `0x04`). Those filters and pin assignments are example-specific; the component itself forwards input reports unchanged.

## Use

The example YAML fetches this component from `https://github.com/deisterhold/BLE_HID_Host`, branch `main`, under `esphome/components`. Push the component to that branch before compiling the example. Configure the original ESP32 with the ESP-IDF framework and the Classic Bluetooth HID Host sdkconfig options shown in the example. `mac_address` is an optional scan filter; when omitted, the component attempts the first discovered Bluetooth peripheral and reconnects after disconnect/failure. The example defines 8-pixel RGBW strips on GPIO 18 and GPIO 21 and handles yellow blinking/cancellation entirely in YAML.

The component must be the only owner of the ESP32 Bluetooth controller. Do not combine it with ESPHome BLE tracker/client/proxy components. Pairing confirmation is accepted automatically, and legacy PIN requests use `0000`.

ESPHome CLI was not installed in this workspace, so the component and example YAML have not been validated with `esphome compile` here. The existing native ESP-IDF firmware remains unchanged.
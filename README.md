# EPaperClockESP32

ESP32-C3 WiFi e-paper clock for a WeAct 2.13" black-and-white e-paper
panel.

This project was originally developed to test an ePaper display together
with an ESP32. If you build on it, a mention or link back to this repo would
be appreciated.

## Hardware

- ESP32-C3 (4 MB flash)
- WeAct 2.13" e-paper, 250x122, B/W, SSD1680
- WiFi connection for NTP sync
- Optional wake button on GPIO1 to GND (Not really tested. Used only RESET) 
- Optional WS2812 RGB LED on a configured GPIO pin Blinks once Green when awake from deep sleep

Wiring used in the current build:

- CS = GPIO10
- DC = GPIO4
- RST = GPIO3
- BUSY = GPIO5
- SCK = GPIO6
- MOSI = GPIO7
- VCC = 3V3
- GND = GND

## Features

- Landscape clock with a configurable header
- Big HH:MM time display
- Date line below the time
- Partial refresh for the changed regions only
- Full refresh on boot, wake, and after a number of partial updates
- WiFi-based NTP sync
- NVS configuration for stored WiFi entries and settings
- Serial command interface for WiFi, time, header, rotation, and power mode
- Deep sleep support with a full redraw after wake  
   When in deep sleep "5V 0,00A 0,000W" it wakes for each minute update from RTC
   for only a couple sec when the power becomes awake "5V 0,06A 0,29W"

## Serial commands

The firmware exposes a serial command interface on 115200 baud.
Example commands include:

- `help`
- `wifi`
- `scan`
- `list`
- `add wifi <ssid> <pw>`
- `connect <n|ssid>`
- `forget <n|ssid>`
- `sync`
- `time`
- `zone [<+/-N[.N]>]`
- `header [<text>]`
- `frame`
- `rot [0-3]`
- `power normal|light|deep`

https://youtu.be/ctpwPFDf3Ec  shows the screen appearance including 
rot and frame commands

## WiFi setup

This project does not use a separate secrets file.

Use the serial commands to manage WiFi settings:

- `scan` to discover nearby networks
- `add wifi <ssid> <pw>` to save a WiFi entry to NVS
- `list` to show saved SSIDs
- `connect <n|ssid>` to connect to a saved network
- `forget <n|ssid>` to remove a saved network

Notes:

- The current command parser expects the SSID and password as simple single
  tokens. If an SSID contains spaces, it is not handled correctly in the
  current command format.
- For best results, use SSIDs without spaces until the parser is updated to
  support quoted strings.

## Arduino IDE

This project is built and uploaded using the Arduino IDE.
Start with the .ino file that is inside the folder with same name.
Set board to ESP32C3 Dev Module
Set IDE tools to: USB CDC on boot: enabled and
                  Integrated USB Jtag 
Serial monitor: new line; 115 200 baud
Libraries needed: ESP32 expressif, GxEPD2 zingg                     

No PlatformIO configuration is used for this version.

## Power modes

- `normal`: CPU awake, WiFi enabled
- `light`: light sleep, not recommended for NTP reliability
- `deep`: deep sleep until the next scheduled update

## Notes

- The code is optimized for partial refresh behavior.
- The display is bistable, so the last good image remains visible if an
  update fails.
- The panel layout is tuned for landscape mode.
- The code stores settings in NVS and survives reboots.

## License

This project is licensed under the MIT License. See the `LICENSE` file for
details.

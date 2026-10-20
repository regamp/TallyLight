# Native ESP-IDF firmware

This is a separate, Arduino-free ESP-IDF implementation for ESP32-C3 and ESP32-S3. It does not replace the existing Arduino firmware.

## Configure and build

Open a terminal in this directory, configure the target and initial application settings, then build and flash:

```sh
pio run -e ESP32_C3_IDF -t menuconfig
pio run -e ESP32_C3_IDF
pio run -e ESP32_C3_IDF -t upload
```

Use `ESP32_S3_IDF` instead for an ESP32-S3. In `ATEM tally light`, set the initial Wi-Fi SSID/password, ATEM switcher IPv4 address, tally source number, LED modes, and initial Neopixel GPIO. The WS2812B data GPIO and byte order (GRB, RGB, BRG, RBG, BGR, or GBR) can be selected on the setup web page under `Addressable LED strip`; changes are saved to NVS and applied after restart. After boot, other settings can also be changed on the device setup page, saved to NVS, or provisioned over Improv Serial on UART0 at 115200 baud. If saved Wi-Fi is unavailable, the device starts a `Tally Light setup` access point after 10 seconds. Check your board pinout and LED power requirements before connecting the strip.

LED modes are `1` normal, `2` preview stays on, `3` program only, and `4` on-air. All tally and connection status indication is on the WS2812B strip; no discrete RGB GPIO LEDs are used.

LiPo monitoring can be enabled in the setup page. Configure the ADC GPIO, low-voltage threshold in millivolts, and the voltage-divider scale. For example, a 100 kOhm / 100 kOhm divider has a scale of `2.000`; a 100 kOhm / 47 kOhm divider (battery on the 100 kOhm side) has a scale of about `3.128`. Below the configured threshold the status pixel turns purple. When status position is `None`, the first strip pixel is temporarily reserved for the low-battery warning. Verify ADC wiring and divider ratio with a multimeter before relying on the displayed voltage.

## Included behavior

- Station-mode Wi-Fi connection and reconnect.
- Native HTTP setup page and NVS settings persistence, including fallback setup access point.
- ATEM UDP hello, acknowledgement, tally-by-index and streaming-status handling.
- Addressable WS2812/Neopixel strip output with configurable tally and status LED positions.
- Optional calibrated ADC LiPo monitoring, adjustable divider ratio/low-voltage threshold, and purple low-battery indication.
- Web-controlled DEBUG tally source that sends Off, Preview, or Program to the local LEDs and connected UDP tally clients, overriding ATEM tally updates while enabled.
- UDP tally relay on port 9910, with up to five clients, handshake, tally forwarding, ACK/retry, keepalive, and disconnect handling.
- Improv Serial device info, Wi-Fi scan, state reporting, and Wi-Fi credential provisioning.

The tally relay shares the same firmware with direct ATEM mode; clients connect to this unit at UDP port 9910. The ATEM client itself uses a separate ephemeral UDP source port. Configure network and LED wiring for your particular board before use.

The DEBUG enable setting is persistent, but its selected tally number and Off/Preview/Program test state are volatile and start Off after reboot.
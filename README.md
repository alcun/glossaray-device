# Glossaray device

Speech translation on the Waveshare ESP32-S3-Touch-AMOLED-1.8 V2.
Hold the upper button to speak, then release it to hear the translation.

The [Glossaray server and web app](https://github.com/alcun/glossaray) use the
same translation protocol and language pairs.

## Hardware

- Waveshare ESP32-S3-Touch-AMOLED-1.8 **V2**, SKU 29957.
- A USB-C data cable.
- A Mac or Linux computer.

The board has a 368 × 448 CO5300 AMOLED display, ES8311 audio codec,
16 MB flash and 8 MB PSRAM. The V1 board uses different display and touch
hardware and is not supported.

## Install

First, configure a [Glossaray server](https://github.com/alcun/glossaray/blob/main/SELFHOSTING.md)
and obtain its HTTPS origin and `GLOSSARAY_DEVICE_TOKEN`.

```sh
git clone https://github.com/alcun/glossaray-device.git
cd glossaray-device
./flash
```

The script locates PlatformIO or attempts to install it using Homebrew, pipx
or pip. If automatic installation fails, follow the
[PlatformIO installation guide](https://platformio.org/install/cli).
Linux users may also need [USB permissions](https://docs.platformio.org/en/latest/core/installation/udev-rules.html).

Enter your server's HTTPS origin and device token. The script checks server
health and device authentication, lists connected boards, and asks before
uploading. Connect only the board you intend to flash.

The first build downloads the toolchain and libraries. If upload does not
start, hold BOOT, tap RESET, release BOOT, and retry. Press RESET after upload
if the display stays blank.

## Wi-Fi setup

1. Join `GLOSSARAY-SETUP` from your phone.
2. Select your Wi-Fi network and enter its password.
3. Save the settings and wait for the board to connect.

The server and token supplied during flashing are applied once on first boot.
Saved Wi-Fi is preserved, and subsequent changes through setup remain effective.

The firmware connects on HTTPS port 443 and trusts ISRG Root X1. The server
must present a certificate chain trusted by that root. Other certificate
roots or custom ports require firmware changes.

## Controls

| Action | Result |
|---|---|
| Hold upper button, speak, release | Translate speech |
| Tap upper button | Repeat the last translation |
| Press lower button | Change language |
| Double-tap lower button | Reverse translation direction |

## Credentials and audio

The Gemini API key stays on the server. The device stores a bearer token that
grants access to that server; treat it as a credential.

Audio is streamed through the server to Google. The device buffers returned
audio and text in RAM for playback and repeat. It does not save recordings to
flash. Provider-side processing is subject to Google's terms.

Flashing provisions the token into the compiled firmware. Do not distribute
provisioned binaries or build output containing your credential. Build a generic
image without provisioning values when preparing firmware for redistribution.

## Development

With PlatformIO installed:

```sh
pio run
pio device list
pio run --target upload
pio device monitor
```

A direct build has no server or token configured. Configure the device through
setup, or use `./flash` to supply provisioning values.

For automation, the flash script accepts `GLOSSARAY_API_URL`,
`GLOSSARAY_DEVICE_TOKEN`, `GLOSSARAY_UPLOAD_PORT` and
`GLOSSARAY_FLASH_CONFIRM=y`. These values are read from the environment;
provisioning credentials are included in build artifacts.

## Licence

The firmware is [GPL-3.0 licensed](LICENSE). Dependencies and adapted hardware
code retain their notices in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
The separate Glossaray server and website are MIT licensed.

# Third-party notices

## ArduinoWebsockets

Glossaray Device depends on
`gilmaimon/ArduinoWebsockets` version 0.5.4 through PlatformIO.

- URL: <https://github.com/gilmaimon/ArduinoWebsockets>
- Version: `0.5.4`
- License: GPL-3.0

This repository is GPL-3.0 and includes the complete licence text in `LICENSE`.

## Chat Stick

The Glossaray Waveshare V2 QSPI pin map and AXP2101 initialization are selectively
adapted from:

- Project: `steveruizok/chat-stick`
- URL: <https://github.com/steveruizok/chat-stick>
- Commit: `3321c9bfc9771ee8b3adc4815f6c72890d3db125`
- Copyright © 2026 Steve Ruiz
- License: MIT

```text
MIT License

Copyright (c) 2026 Steve Ruiz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Adapted components include the V2 QSPI, I2S and amplifier pin assignments,
power-management initialisation and audio setup.

## U8g2 and GNU Unifont

Glossaray depends on U8g2 version 2.36.18 through PlatformIO to enable the UTF-8
font adapter already supported by Arduino_GFX.

- U8g2: <https://github.com/olikraus/u8g2>, version `2.36.18`, BSD-2-Clause
- GNU Unifont: <https://unifoundry.com/unifont/>, version `17.0.03`
- Font license: GPL-2.0-or-later with the GNU font embedding exception

The complete font data and its upstream license header are distributed inside
the pinned Arduino_GFX 1.6.5 dependency. The embedding exception permits the
font to be embedded in a document without that act alone applying the GPL to
the resulting document; distribution must retain the applicable font notices.

## Waveshare ESP32-S3-Touch-AMOLED-1.8

GPIO46 amplifier control and 16-bit stereo configuration reference
Waveshare's Arduino example:

- URL: <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8>
- Commit: `ba32b5cbca96f0e04b0736d04959b6e832268d3f`
- File: `examples/arduino-v2/examples/15_ES8311/15_ES8311.ino`
- License: Apache-2.0

## Espressif ES8311 driver

The ES8311 register sequence is derived from the
driver retained by the pinned Chat Stick commit:

- File: `devices/firmware/waveshare/src/drivers/es8311/es8311.c`
- Copyright: 2015–2022 Espressif Systems (Shanghai) CO LTD
- SPDX license identifier: Apache-2.0
- Upstream license: <https://www.apache.org/licenses/LICENSE-2.0>

The adapted code configures 24 kHz, 16-bit audio. Source attribution is in
`src/audio.cpp`.

## GFX Library for Arduino

Glossaray depends on `moononournation/GFX Library for Arduino` version 1.6.5 through
PlatformIO.

- URL: <https://github.com/moononournation/Arduino_GFX>
- Tag: `v1.6.5`
- Commit: `4c1d0a6b3a4ea7999c2e607a8c306d30eb7c2ac2`
- License: BSD

```text
Software License Agreement (BSD License)

Copyright (c) 2012 Adafruit Industries.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

Upstream repository:
<https://github.com/moononournation/Arduino_GFX>.

## XPowersLib

Glossaray depends on `lewisxhe/XPowersLib` version 0.3.3 through PlatformIO.

- URL: <https://github.com/lewisxhe/XPowersLib>
- Tag: `v0.3.3`
- Commit: `ef40b49af66fe3b1907ab74ca76e10c0dbf69012`
- License: MIT
- Copyright © 2022 Lewis He

The installed package includes its complete MIT license. Upstream repository:
<https://github.com/lewisxhe/XPowersLib>.

## Additional dependencies

| Component | Version | Licence |
|---|---|---|
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.4.2 | MIT |
| [WiFiManager](https://github.com/tzapu/WiFiManager) | 2.0.17 | MIT |

PlatformIO installs the Arduino ESP32 framework and its components, which retain
their upstream notices. See `platformio.ini` for the build configuration.

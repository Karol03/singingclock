# Random WAV player on XIAO ESP32S3 + SD + RTC
Project for Arduino IDE 2.3.6. Battery-powered device, on a random day from a given range, at a random hour and minute from a given range, plays one random .wav file from a microSD card.

## Table of contents
1. [Overview](#Overview)
2. [Hardware](#Hardware)
3. [Wiring Overview](#Wiring-Overview)
4. [SD Card Structure](#SD-Card-Structure)
5. [Audio File Format](#Audio-File-Format)
6. [Configuration (config.txt)](#Configuration-(config.txt))
7. [Time setting (timestamp.txt)](#Time-setting-(timestamp.txt))
8. [External libraries](#External-libraries)
9. [License](#License)

## Overview
The device plays one random WAV file at a random time within the specified ranges:
- day – randomly selected from a date range (1..28) or day of week (1..7),
- hour – randomly selected from a range (0..23),
- minute – randomly selected from a range (0..59).

The ranges are configurable in `config.txt`. The time is based on DS3231 with optional daylight saving time (DST) correction set in the configuration file.

## Hardware
- MCU: Seeed XIAO ESP32S3
- Audio/SD: Adafruit 5769 SD card amplifier module (SD card formatted in FAT32 system)
- RTC: DS3231 (accurate real-time clock)
- IRFZ44N - N-channel MOSFET to drive Adafruit 5769 power supply


## Wiring Overview
I²C (RTC DS3231):

    SDA → SDA pin on XIAO
    SCL → SCL pin on XIAO
    INT/SQW → A0 pin on XIAO
    Power: 3V3 & GND

SPI (SD card on Adafruit 5769):

    MOSI, MISO, SCK → SPI pins on XIAO
    CS_SD → TX (GPIO43) pin on XIAO

I²S (to amplifier on Adafruit 5769):

    BCLK → I²S BCK pin on XIAO (A3)
    LRC / WS → I²S LRCLK pin on XIAO (A2)
    DIN → I²S data pin on XIAO (A1)
    5V → XIAO 5V 
    3.3V → XIAO 3.3V
    GND → N-MOSFET drain

N-MOSFET:

    SOURCE → GND
    DRAIN → Adafruit 5769 GND
    GATE → GPIO_NUM_44 (by 100 Ohm)
    GND → GATE (by 10 kOhm)
    

## SD Card Structure
SD card structure

    / (card root)
    ├── config.txt        # configuration of ranges and DST
    ├── timestamp.txt     # optional, to update RTC time, only EPOCH (UTC+0)
    └── sound1.wav
    └── sound2.wav
    └── sound3.wav
    └── ...

WAV file names can be arbitrary. The player randomly selects from all .wav files in the SD card.

## Audio file format
- WAV PCM 16-bit, mono
- Recommended sampling rate 44100 Hz
- Avoid metadata/extensions that are unusual for simple decoders on microcontrollers.

## Configuration (config.txt)
Text file config.txt (UTF-8). For example see `example_config.txt` file.

## Time setting (timestamp.txt)

To set the RTC, create a file called `timestamp.txt` in the root directory containing only one number – the epoch time (seconds since 1970-01-01 00:00:00 UTC).

Example (UTC+0):

1765584000

After the restart, the device will read the value, set the DS3231 and delete the file so as not to overwrite the time with each reset.

    Remember: timestamp.txt must be in UTC+0. Zone/DST corrections are handled by config.txt.

## External libraries
- https://github.com/DFRobot/DFRobot_MAX98357A/ - Modified version to handle Adafruit 5769 shield
- https://github.com/NorthernWidget/DS3231 - Modified version with corrected DoW to handle RTC

## License
This project is licensed under the MIT License – see the [LICENSE](LICENSE) file for details.

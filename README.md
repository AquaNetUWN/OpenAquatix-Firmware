# License

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

# OpenAquatix Firmware
Firmware for the OpenAquatix: a JANUS compatible software defined underwater acoustic modem for research purposes

# Description
The firmware in this repository is for the underwater acoustic modem found [here](https://github.com/ericvoi/UAM_PCB/tree/main).

# Building
This repository now supports a side-by-side CMake build in addition to the existing STM32CubeIDE project files.

## Requirements
- CMake 3.31+
- Ninja
- `arm-none-eabi-gcc` toolchain available on `PATH`

## CMake Build
Configure and build the debug firmware:

```sh
cmake --preset debug
cmake --build --preset debug
```

Configure and build the release firmware:

```sh
cmake --preset release
cmake --build --preset release
```

Build outputs are written to `build/debug` or `build/release` and include:
- `UAM.elf`
- `UAM.bin`
- `UAM.list`
- `UAM.map`

# Key Features
- 6 types of error correction (CRC-8, CRC-16, CRC-32, checksum-8, checksum-16, checksum-32)
- FSK and FHBFSK modulation/demodulation schemes
- K=9 convolutional code
- Fully JANUS compliant
- Feedback networks for both the input and output networks to ensure that the system is calibrated

# Application Overview
The firmware for this project consists of a six-task FreeRTOS application that manages modulation, demodulation, external communication over USB or UART, system monitoring, medium access control, and storing configuration data.

## Message Processing (MESS)
This task handles all of the signal processing for both the input and output as well as handling the feedback networks which ensure calibration of the device. Task functions:
- Listening to the input ADC to determine when a message starts
- Decoding received messages
- Preparing packets with a sender id, message type, and message length
- Adding error correction to packets and determining if errors occurred during demodulation
- Printing raw data over USB
- Calibrating the input hardware and the output hardware to ensure responsivity over frequency (TODO)
- Sending received messages to the COMM task

## Communication (COMM)
This task serves as the communication link for users and hosts a HMI over USB and UART. Task functions:
- Hosting a HMI that lets the user change internal parameters, invoke functions, and view parameters
- Printing received messages
- Outputting HMI over USB and UART and listening for commands from either interface

## System (SYS)
This task is the central task and its primary purpose is to ensure that the system is operating as expected. Task functions:
- Track power consumption (TODO)
- Track temperature
- Track errors (TODO)
- Check misc input GPIO pins (TODO)
- Update status LED according to system state
- Determine overall system state and relay that to other tasks (TODO)
- Act as the sole task in low-power modes

## Configuration (CFG)
This task facilitates the storage of all configuration parameters in flash memory. Task functions:
- Load parameters from flash on boot
- Update changed parameters to flash

## Medium Access Control (MAC)
This task can either run local MAC logic or operate in a host-controlled mode for an attached Raspberry Pi:
- Local modes remain available for no-MAC and CSMA/CA with BEB
- Host-controlled mode bypasses local contention logic and lets the Pi own slotting, reservations, retries, and transmit timing decisions
- Scheduled host transmissions are handed off to the MESS task using the existing delayed-start path
- Channel reports can be forwarded as machine-readable sensing telemetry for Pi-side MAC logic
- Received messages continue to flow to the COMM task for human-readable or machine-readable output

## DAC (DAC)
This task's only purpose is to fill the DAC DMA buffers when notified by the DMA callback. Task functions:
- Modulating the DAC with DMA to generate an input signal for the power amplifier

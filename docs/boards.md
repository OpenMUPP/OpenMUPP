# Boards

This document describes the board support of the firmware: the board definitions, the start-up and the LEDs.
The README lists the supported boards.

## MCU family

The board selects the MCU family of the dongle.
The firmware knows two families: STM32F1 and STM32H5.
The Mini, the OtherUse and the ST-Link/V2 use an STM32F103x8 or STM32F103xB (family STM32F1).
CMSIS has one device define (`STM32F103xB`) for both parts.
The NUCLEO-H533RE uses an STM32H533RE (family STM32H5, see [NUCLEO-H533RE](#nucleo-h533re)).

`cmake/Boards.cmake` has one line for each board, with these values:

| Value | Use |
|---|---|
| Family | `STM32_FAMILY` of system-stm32. The toolchain file `cmake/gcc_arm.cmake` sets the CPU flags from it. |
| Device | `STM32_DEVICE` of system-stm32, the CMSIS device define. |
| Crystal | `SYSTEM_STM32_HSE_VALUE` of system-stm32, in Hz. 0 is a board without a crystal. |
| Linker script | The memory linker script in `ldscripts/`, for example `mem-f103.ld`. |
| Flash | The flash size in bytes. The size limit of each application image comes from it. The STM32F1 boards use 64 KB, the flash of the STM32F103x8. |

`CMakeLists.txt` reads `cmake/Boards.cmake` before `project()`.
The toolchain file needs `STM32_FAMILY` before the compiler checks start.
`cmake/gcc_arm.cmake` has CPU flags for the STM32F1 (Cortex-M3) and the STM32H5 (Cortex-M33).
Both families use soft float, as system-stm32 and libart-stm32.

## Board definitions

Each board has one `constexpr` board definition in `src/board/boards/`.
The board definition holds all board-specific data:

- The pin map: JTAG and SWD, UART, Vref sense, power switch, LEDs, key and USB pull-up.
- The MCU family.
- The crystal frequency. The value 0 is a board without a crystal. Only an STM32H5 board can have no crystal: its USB clock comes from the HSI48 and the clock recovery system (CRS).
- The JTAG fast path (SPI and DMA, see [JTAG fast path](usb-device.md#jtag-fast-path)). It is only available when the board links TCK (PB13) to PB3.
- The hardware ID that `VERSALOON_GET_HARDWARE` returns.

`src/board/BoardDefinition.h` defines the types. It does not use CMSIS, so the host unit tests check all boards.
CMake defines `VSL_BOARD_<BOARD>` (for example `VSL_BOARD_MINI`), and `src/board/Board.h` selects `board::current` from it.
The protocol and the engines read board data only through `board::current`.
`src/board/BoardPins.h` maps a board pin to the system-stm32 `GpioPin` and `FastPin` drivers.

The family and the crystal share one 32-bit word.
Thus a board definition stays 68 bytes, the same size as before the family field.
The firmware keeps the definition of the board in the flash.

Some functions come from the pin map and the family, not from a separate field:

- `board::adcChannelOf()` gives the ADC channel of the Vref pin.
  The channels of the STM32F103 come from RM0008. The channels of the STM32H523/H533 come from DS14539.
- `board::oneWireTimerOf()` gives the timer of the one-wire engine (WCH-Link) on the SWDIO output pin.
  The STM32H5 has no one-wire engine yet, so this function gives no timer for it.
- `board::reservedPins()` gives the pins that no signal can use: USB D- and D+ (PA11, PA12) and the crystal pins.
  The crystal pins are PD0 and PD1 on the STM32F103, and PH0 and PH1 on the STM32H5.
- `board::swjConfig()` gives the debug port (SWJ) setting of the dongle MCU.
  The setting frees all debug port pins that the board uses, and keeps the rest.
  The Mini and the OtherUse use PA14 for an LED, so they disable the debug port.
  The ST-Link/V2 keeps JTAG and SWD.
  The debug port pins are the same on the two families.
  The STM32F103 frees the pins with `AFIO_MAPR.SWJ_CFG`.
  The STM32H5 has no AFIO. Its debug port pins are in AF0 after reset, and the GPIO mode of a pin frees the pin.
- `board::withSwd()` gives the board of a debug build with `VSL_DEBUG_SWD`.
  It removes the LEDs on the SWD pins (PA13, PA14), so the debug port keeps SWD.
  On the Mini and the OtherUse, the USB LED (PA14) is then not available.

The build fails when a board definition is not valid.
Examples are a pin that is used two times, or a crystal that is different from `SYSTEM_STM32_HSE_VALUE`.
The build also fails when the family of the board definition is different from `STM32_FAMILY`.
The JTAG fast path has fixed pins. TMS is on SPI1 MISO: PB4 on the STM32F103, PA6 on the STM32H5.
PB4 can also be SPI1 MISO on the STM32H5. The pin map of the H5 boards uses PB4 for TRST, so the firmware uses PA6.
The crystal range is 4 MHz to 16 MHz on the STM32F103, and 4 MHz to 50 MHz on the STM32H5.

To add a board:

1. Add a header with the board definition in `src/board/boards/`.
2. Add the board to `src/board/Board.h`.
3. Add the board to `VSL_BOARDS` in `cmake/Boards.cmake`, with its family, device, crystal and linker script.
   A new family also needs CPU flags in `cmake/gcc_arm.cmake`.
4. Add a build job to `.gitlab-ci.yml` and a test to `tests/TestBoards.cpp`.

## Start-up and LEDs

The system-stm32 start-up code calls `os_startup_initialize_hardware()` (`src/initialize_hardware.cpp`) before `main()`.
This function does these steps:

1. It sets the system clock from the board crystal, and the SysTick to 1 ms.
   The STM32F1 runs at 72 MHz.
   The STM32H5 runs at 250 MHz, with the instruction cache on.
2. It sets the debug port (SWJ) of the dongle MCU from `board::swjConfig()`.
   The Mini and the OtherUse disable the debug port. A debug probe can then connect only under reset.
   With `VSL_DEBUG_SWD` (default in a `Debug` build), SWD stays on and JTAG is off.
   Then a debug probe can run and step the firmware.
3. It puts the board pins into a safe state:
   - All LEDs are off, but the power LED is on. The main loop then drives the LEDs (see below).
   - The USB pull-up is off, so the host does not see the device.
   - The target power is off (the switch pin floats).
   - The key pin has a pull resistor to the released level.

The JTAG, SWD, UART and Vref pins stay inputs until their drivers configure them.

`main()` starts the USB device when the clock comes from the crystal (see [USB device](usb-device.md)).
The STM32F1 gets the USB clock from the PLL.
The STM32H5 gets the USB clock from the HSI48, and the clock recovery system (CRS) trims it from the USB start-of-frame packets.
The main loop runs the USB device and the UART bridge. It also drives the LEDs.
A `wch-link` image has no serial port and no UART bridge (see [WCH-Link personality](wch-link.md)).

| LED | Meaning |
|---|---|
| Power | On when the Vref pin has a voltage above 1.8 V. Off when the voltage is below 1.5 V. The dongle measures the voltage each 100 ms. |
| State | Off when the dongle is idle. One flash of 40 ms for each command of the host (Versaloon, CMSIS-DAP or WCH-Link). |
| USB | On when the host has configured the USB device. |

The power LED shows the target voltage, from the target supply or from the dongle supply.
A board without a working Vref sense keeps the power LED on all the time. This includes a board where the ADC calibration failed.
The dongle has no idle indication of the firmware. An idle dongle with no target and no USB host has all LEDs off. A hung main loop shows the same.
A continuous command stream gives a steady flicker on the state LED, because the LED stays off for at least 40 ms after each flash.

Three fault and event patterns replace the normal function:

| LED | Pattern | Meaning |
|---|---|---|
| State | Double flash: two flashes of 60 ms, each 600 ms | A JTAG or SWD transfer failed. The pattern starts at the first error and stops 2 s after the last error. Each new error restarts the 2 s. During the pattern, the state LED does not show the command activity. |
| State | Fast flash, five times each second | The crystal or the PLL did not start. The clock is HSI (8 MHz) and USB cannot work. The power LED still shows the target voltage. |
| All | Three flashes in 0.6 s, then on | The key has been down for 3 s: the dongle switches the personality of a multi image. After the flashes, all LEDs stay on until you release the key. The dongle then resets. |

A probe error is a transfer that ends with a FAULT acknowledge, no acknowledge, a parity error, or a WAIT acknowledge after all retries.
In the CMSIS-DAP personality, these are the transfers of `DAP_Transfer` and `DAP_TransferBlock`, on SWD and on JTAG.
A JTAG-DP has one acknowledge value for OK and FAULT.
A fault on JTAG only sets the STICKYERR bit in CTRL/STAT, and the dongle does not read this bit.
Thus on JTAG, only these errors count: a WAIT after all retries, an acknowledge that is not valid, and a failed scan.
A WAIT that `DAP_TransferAbort` stops and a value mismatch are not probe errors.
In the Versaloon personality, these are the failed `IN_OUT` transfers of the `SWD` block.
The `JTAG_RAW` block has no acknowledge, so the Versaloon personality does not show JTAG errors.
A host can cause probe errors in normal operation, for example when it scans for access ports that are not there.
Thus a short double flash at the connection is not always a problem.
The WCH-Link personality counts no probe errors, so it never shows the double flash.

The reset of the switch waits until the key is up for 50 ms, because a key that is down at the reset starts the bootloader.
The bootloader (versaloon-bootloader) keeps its own LED patterns, and the patterns of this firmware do not change them.
A build with `VSL_DEBUG_SWD` has no USB LED on the Mini and the OtherUse.

## NUCLEO-H533RE

The board `nucleoh533` is the NUCLEO-H533RE (MB1814, user manual UM3121).
It has an STM32H533RET6 and a 24 MHz crystal.
The pin map is the pin map of the new probe board with the STM32H523, so the two board definitions are almost the same.

The Nucleo has no level shifters and no target power switch.
Connect only targets with a 3.3 V supply.
The Nucleo and the target must have a common ground.

The firmware on the Nucleo has these limits:

- One personality for each image: Versaloon or CMSIS-DAP. A multi image needs a settings sector (#93). The build stops with an error for a multi image.
- No WCH-Link personality. The one-wire engine of the STM32H5 is #98. The build stops with an error for `wch-link`.
- Only the `standalone` image at `0x08000000`. `VERSALOON_FW_UPDATE` refuses the request (see [Firmware update](firmware-update.md)).
- Bit-bang JTAG only. The JTAG fast path is #97.
- The timing of the bit-bang engines is not yet measured at 250 MHz (#94).

### Wiring to the target

The pins are on the ST morpho connectors CN7 and CN10.
Some pins are also on the ARDUINO connectors.

| Signal | Pin | Connector | Note |
|---|---|---|---|
| TCK, SWCLK | PB13 | CN10 pin 30 | |
| TMS, SWDIO (output) | PA6 | CN10 pin 13 (D12) | Connect a wire from PA6 to PA7. Connect the target to one of the two pins. |
| TMS, SWDIO (input) | PA7 | CN10 pin 15 (D11) | |
| TDI | PB15 | CN10 pin 26 | |
| TDO | PB14 | CN10 pin 28 | |
| TRST | PB4 | CN10 pin 27 (D5) | Optional |
| SRST | PB10 | CN10 pin 25 (D6) | |
| RTCK | PA0 | CN7 pin 28 (A0) | Optional |
| UART TX (to target RX) | PA9 | CN10 pin 2 | USART1 of the serial bridge |
| UART RX (from target TX) | PA10 | CN10 pin 4 | |
| Vref sense | PB0 | CN7 pin 34 (A3) | Optional. Connect the target supply, 3.3 V maximum. No divider. |
| GND | GND | CN7 pin 8 | |

The default solder bridges of the NUCLEO-H533RE also connect PB14 and PB15 to CN10 pins 35 and 37, and to the ARDUINO pins D1 and D0.

The board has these other signals:

| Signal | Pin | Note |
|---|---|---|
| State LED | PA5 | LD2, green. The board has no power LED and no USB LED. |
| Key | PC13 | B1, the blue button. |
| USB | PA11, PA12 | USB Type-C connector CN3. The D+ pull-up is in the STM32H5. |

Without a Vref connection, the Vref value of the Versaloon personality is not valid.

### Flash the firmware

Connect the USB connector of the on-board STLINK-V3EC (CN1).
Write the `standalone` image with the STLINK-V3EC, for example:

```sh
openocd -f interface/stlink.cfg -f target/stm32h5x.cfg \
        -c "program versaloon-ng-standalone.bin 0x08000000 verify reset exit"
```

Then connect the user USB connector CN3 to the host.
The firmware keeps the SW-DP of the STM32H5 on, so the STLINK-V3EC can connect at any time.
The STLINK-V3EC uses PA13 and PA14. The firmware does not use these pins.

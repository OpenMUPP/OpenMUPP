# Boards

This document describes the board support of the firmware: the board definitions, the start-up and the LEDs.
The README lists the supported boards.

All boards use an STM32F103x8 or STM32F103xB.
CMSIS has one device define (`STM32F103xB`) for both parts.
The build sets `SYSTEM_STM32_HSE_VALUE` of system-stm32 from the board crystal.

## Board definitions

Each board has one `constexpr` board definition in `src/board/boards/`.
The board definition holds all board-specific data:

- The pin map: JTAG and SWD, UART, Vref sense, power switch, LEDs, key and USB pull-up.
- The crystal frequency.
- The JTAG fast path (SPI and DMA, see [JTAG fast path](usb-device.md#jtag-fast-path)). It is only available when the board links TCK (PB13) to PB3.
- The hardware ID that `VERSALOON_GET_HARDWARE` returns.

`src/board/BoardDefinition.h` defines the types. It does not use CMSIS, so the host unit tests check all boards.
CMake defines `VSL_BOARD_<BOARD>` (for example `VSL_BOARD_MINI`), and `src/board/Board.h` selects `board::current` from it.
The protocol and the engines read board data only through `board::current`.
`src/board/BoardPins.h` maps a board pin to the system-stm32 `GpioPin` and `FastPin` drivers.

Some functions come from the pin map, not from a separate field:

- `board::adcChannelOf()` gives the ADC channel of the Vref pin.
- `board::swjConfig()` gives the debug port (SWJ) setting of the dongle MCU.
  The setting frees all debug port pins that the board uses, and keeps the rest.
  The Mini and the OtherUse use PA14 for an LED, so they disable the debug port.
  The ST-Link/V2 keeps JTAG and SWD.
- `board::withSwd()` gives the board of a debug build with `VSL_DEBUG_SWD`.
  It removes the LEDs on the SWD pins (PA13, PA14), so the debug port keeps SWD.
  On the Mini and the OtherUse, the USB LED (PA14) is then not available.

The build fails when a board definition is not valid.
Examples are a pin that is used two times, or a crystal that is different from `SYSTEM_STM32_HSE_VALUE`.

To add a board:

1. Add a header with the board definition in `src/board/boards/`.
2. Add the board to `src/board/Board.h`.
3. Add the board and its crystal to `VSL_BOARDS` and `SYSTEM_STM32_HSE_VALUE` in `CMakeLists.txt`.
4. Add a build job to `.gitlab-ci.yml` and a test to `tests/TestBoards.cpp`.

## Start-up and LEDs

The system-stm32 start-up code calls `os_startup_initialize_hardware()` (`src/initialize_hardware.cpp`) before `main()`.
This function does these steps:

1. It sets the system clock to 72 MHz from the board crystal, and the SysTick to 1 ms.
2. It sets the debug port (SWJ) of the dongle MCU from `board::swjConfig()`.
   The Mini and the OtherUse disable the debug port. A debug probe can then connect only under reset.
   With `VSL_DEBUG_SWD` (default in a `Debug` build), SWD stays on and JTAG is off.
   Then a debug probe can run and step the firmware.
3. It puts the board pins into a safe state:
   - All LEDs are off, but the power LED is on.
   - The USB pull-up is off, so the host does not see the device.
   - The target power is off (the switch pin floats).
   - The key pin has a pull resistor to the released level.

The JTAG, SWD, UART and Vref pins stay inputs until their drivers configure them.

`main()` starts the USB device when the clock comes from the crystal (see [USB device](usb-device.md)).
The main loop runs the USB device and the UART bridge. It shows a heartbeat on the state LED:

| State LED | Meaning |
|---|---|
| Short flash, one time each second | The firmware runs. The clock comes from the crystal. |
| Fast flash, five times each second | The crystal or the PLL did not start. The clock is HSI (8 MHz) and USB cannot work. |
| On or off all the time | The main loop does not run. |

The USB LED is on when the host has configured the USB device.
A build with `VSL_DEBUG_SWD` has no USB LED on the Mini and the OtherUse.


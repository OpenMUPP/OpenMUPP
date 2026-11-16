# AVR programming

The CMSIS-DAP personality can program AVR chips through the ISP (SPI) interface.
ISP programming passed a hardware test with avrdude 8.3 on the YD-CH32V307VCT6 board (see [Hardware test](#hardware-test)).
Tools that use the Microchip EDBG protocol, for example avrdude, find the probe over USB.
The probe does not have debugWIRE or other AVR interfaces yet.

## Build

Use an image with the build option `MUPP_CMSIS_DAP_AVR`.
This option needs `MUPP_CMSIS_DAP_HID`.
Only the CH32V307 boards have the option (see [cmake.md](cmake.md)).
The presets `ydch32v307-cmsis-dap-avr` and `ydch32v307-all-avr` set both options.
The protocol is in [cmsis-dap.md](cmsis-dap.md#avr-commands).

## USB ID

The probe has the USB ID `1209:0001` (the test ID of pid.codes) and the product name `OpenMUPP CMSIS-DAP`.
The product name must contain `CMSIS-DAP`, because avrdude uses it to select the packet size.

## Linux access

avrdude uses the HID interface of the probe.
Copy `etc/70-openmupp-avr.rules` to `/etc/udev/rules.d/`.
The rule gives a normal user access to the `hidraw` device of the probe.
Then run `udevadm control --reload` and plug in the probe again.

The file `etc/70-openmupp-cmsis-dap.rules` gives access to the USB device, for the other CMSIS-DAP tools (see [cmsis-dap.md](cmsis-dap.md#use-the-probe)).

## Wiring

The ISP signals use the pins of the JTAG port.
All signals are 3.3 V. Connect only a target that runs at 3.3 V, or use an external level shifter.
The YD board has no target power switch: the target needs its own supply.
Connect the target GND to the probe GND.

### YD-CH32V307VCT6 board

The [pinout image](boards.md#yd-ch32v307vct6-development-board) shows the ISP pins.

![Wiring of the 6-pin ICSP header of the target to the YD-CH32V307VCT6 board](images/ydch32v307-avr-isp.svg)

| ISP signal | Probe pin | Header pin | 6-pin ICSP header of the target |
|---|---|---|---|
| SCK | PB13 | P3-45 | 3 |
| MOSI | PB15 | P3-43 | 4 |
| MISO | PB14 | P3-44 | 1 |
| RESET | PB12 | P3-46 | 5 |
| Target voltage sense | PB0 | P2-29 | 2 (VCC) |
| GND | GND | any GND pin | 6 |

- Connect PB0 to the target VCC. The probe reads the target voltage there. The pin must not get more than 3.3 V.
- RESET is an open-drain output. The AVR chip has an internal pull-up on RESET.
- The JTAG fast path link (PB13 to PC10) is not necessary for ISP.

### OpenMUPP v1 board

The board definition of OpenMUPP v1 (`openmuppv1`) has the AVR pins, but the firmware does not drive the shifter direction pins yet.
Thus the AVR commands do not reach the target on this board today (see [OpenMUPP v1 probe board](boards.md#openmupp-v1-probe-board)).
The adapter that maps the target connector J4 to the ICSP pins is part of the board design.
See the [board repository](https://github.com/OpenMUPP/openmupp-v1) for the pin map of J4.

## avrdude commands

The file `etc/avrdude-openmupp.conf` has a programmer entry with the name `openmupp`.
Read it with `-C +etc/avrdude-openmupp.conf`. Use avrdude version 7.1 or later.
Run the commands in the top directory of the repository, or give the full path of the file.
The entry is not part of avrdude.
The examples are for a target clock of 8 MHz or more. For a slower target, change the `-B` value (see [Low-clock targets](#low-clock-targets)).
A new ATmega328P runs at 1 MHz. For a new chip, use `-B 125kHz` until you change the fuses.

```sh
# Read the signature of an ATmega328P
avrdude -C +etc/avrdude-openmupp.conf -c openmupp -p m328p -P usb -B 2MHz

# Write and verify the flash from an Intel HEX file
avrdude -C +etc/avrdude-openmupp.conf -c openmupp -p m328p -P usb -B 2MHz -U flash:w:firmware.hex:i

# Read the fuses
avrdude -C +etc/avrdude-openmupp.conf -c openmupp -p m328p -P usb -B 2MHz -U lfuse:r:-:h -U hfuse:r:-:h -U efuse:r:-:h

# Write the EEPROM
avrdude -C +etc/avrdude-openmupp.conf -c openmupp -p m328p -P usb -B 2MHz -U eeprom:w:data.hex:i
```

## Low-clock targets

The ISP clock (SCK) must be less than a quarter of the clock of the target.
avrdude sets the SCK only with the `-B` option.
Without `-B`, the probe uses a bit-bang SCK of 100 kHz after power-up.
The probe keeps the last `-B` value until the next power-up, also for the next session.
Thus always give `-B`.

| Target clock | Maximum SCK | Option |
|---|---|---|
| 8 MHz or more | 2 MHz | `-B 2MHz` (the SPI gives 1.125 MHz) |
| 1 MHz (factory setting of many chips) | 250 kHz | `-B 125kHz` |
| 128 kHz (internal watchdog RC oscillator) | 32 kHz | `-B 16kHz` |

- The probe has an SPI engine for SCK from 562.5 kHz to 36 MHz (SPI2 at the 144 MHz APB1 clock, divided by 4 to 256). For a lower SCK, it uses a software engine at the requested rate.
- SCK is never faster than the request.
- A slow SCK makes a write take more time. Use the fastest `-B` value that the target clock permits.
- When a chip does not answer, try a lower SCK. A wrong fuse setting can select a clock that is much slower than you expect.

## Hardware test

The test used a YD-CH32V307VCT6 board and a Seeeduino Lite (ATmega32U4) with a 3.3 V supply.

| Test | Result |
|---|---|
| Read the signature, fuses and lock bits | Pass |
| Read 32 KiB flash and 1 KiB EEPROM at `-B 2MHz` and at `-B 125kHz` | Pass. The two reads are the same. The flash read takes 1.6 s at 2 MHz and 9.2 s at 125 kHz. |
| Erase, write and verify 32 KiB flash at `-B 2MHz` | Pass. The write takes 2.7 s. |
| Write and verify 1 KiB EEPROM at `-B 2MHz` | Pass |
| Write a fuse and read it back | Pass |
| Target at 1 MHz (internal RC oscillator, CKDIV8): flash and EEPROM at `-B 125kHz` | Pass. The flash write takes 10.4 s. At `-B 2MHz`, the probe cannot enter programming mode, as expected. |
| Target at 1 MHz: EEPROM at `-B 16kHz` | Pass |

A target at 128 kHz is not tested: the ATmega32U4 has no 128 kHz clock source.

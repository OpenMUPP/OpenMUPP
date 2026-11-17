# AVR programming

The CMSIS-DAP personality can program AVR chips through the ISP (SPI) interface and through debugWIRE (dW).
ISP programming passed a hardware test with avrdude 8.3 on the YD-CH32V307VCT6 board (see [Hardware test](#hardware-test)).
Tools that use the Microchip EDBG protocol, for example avrdude, find the probe over USB.
avrdude can also program a chip over debugWIRE, and Bloom with avr-gdb can debug it.
debugWIRE passed a hardware test on an ATtiny85 at 8 MHz and at 1 MHz (see [debugWIRE hardware test](#debugwire-hardware-test)).
The probe does not have the other AVR interfaces (JTAG, UPDI, PDI, TPI).
[AVR JTAG on-chip debug protocol](avr-jtag-ocd.md) describes the JTAG debug protocol, for a later JTAG debug function.

## Build

Use an image with the build option `MUPP_CMSIS_DAP_AVR`.
This option needs `MUPP_CMSIS_DAP_HID`.
Only the CH32V307 boards have the option (see [cmake.md](cmake.md)).
The presets `ydch32v307-cmsis-dap-avr` and `ydch32v307-all-avr` set both options.
The protocol is in [cmsis-dap.md](cmsis-dap.md#avr-commands).

## USB ID

The probe has the USB ID `1209:0001` (the test ID of pid.codes) and the product name `OpenMUPP CMSIS-DAP`.
The product name must contain `CMSIS-DAP`, because avrdude uses it to select the packet size.

## Bloom

Bloom is a debug server for avr-gdb.
Each Bloom tool class has a fixed USB ID and a fixed HID interface number.
Bloom has no class for the ID `1209:0001`.
Until Bloom has one, use one of the two ways below.
The steps were checked against Bloom commit `9c066aa`.

### The medbg build

The build option `MUPP_AVR_USB_ID=medbg` gives the probe the ID of the Atmel Xplained Mini (`03eb:2145`).
The build also puts the HID interface at number 0, because Bloom opens the HID interface of the Xplained Mini by this number.
The other interfaces move up by one: CMSIS-DAP v2 is interface 1, and the serial port is interfaces 2 and 3.
On Windows, WinUSB binds interface 1.
The product name stays `OpenMUPP CMSIS-DAP`.

In `bloom.yaml`, set the tool name `xplained_mini`.
The value `xplained_nano` also works, because that class has the same ID and interface number.
Bloom selects the driver by this name, not by the USB ID.
This `bloom.yaml` is for an ATtiny85 over debugWIRE, in the format of Bloom 2.1:

```yaml
environments:
  default:
    shutdown_post_debug_session: true
    tool:
      name: "xplained_mini"
    target:
      name: "attiny85"
      physical_interface: "debug_wire"
      hardware_breakpoints: true
      disable_debug_wire_pre_disconnect: false
    server:
      name: "avr_gdb_rsp"
      ip_address: "127.0.0.1"
      port: 1442
```

Then connect avr-gdb with `target remote 127.0.0.1:1442`.
Set `disable_debug_wire_pre_disconnect` to `true` to turn debugWIRE off at the end of the session (see [Back to ISP](#back-to-isp)).

Use the preset `ydch32v307-cmsis-dap-avr-medbg`, or set `-DMUPP_AVR_USB_ID=medbg` with the options of the preset `ydch32v307-cmsis-dap-avr`.
The option needs `MUPP_CMSIS_DAP_AVR` and the `cmsis-dap` personality alone.
The image name has `-medbg` at the end of its tag.

The vendor ID `03eb` belongs to Microchip.
Use this build for personal use only.
Do not publish or sell an image with this ID.
CI builds the image to check it, and does not publish it.
Other USB tools that use the ID `03eb:2145` can select the probe by mistake.

### A patched Bloom

You can build Bloom for the own USB ID `1209:0001`.
Copy the class `XplainedMini` (`src/DebugToolDrivers/Microchip/XplainedMini/`) to a new class and change these items:

1. In the new `.hpp`, set `USB_VENDOR_ID` to `0x1209` and `USB_PRODUCT_ID` to `0x0001`.
2. Set `CMSIS_HID_INTERFACE_NUMBER` to 3, the HID interface of the default image.
3. Change the text that `getName()` returns.
4. Add the new `.cpp` to the list in `src/DebugToolDrivers/CMakeLists.txt`.
   Without it, the link fails.
5. Add an `#include` for the new class to `src/DebugToolDrivers/DebugTools.hpp`.
6. Add a new lower-case key for the class to the map in `TargetControllerComponent::getSupportedDebugTools()`.
   Set this key as `tool` in `bloom.yaml`.

Then build Bloom.
The patch is for personal use.
An entry in the upstream Bloom repository waits for a USB ID that pid.codes assigns to the project.

### Access rights on Linux

Bloom opens the USB device node with libusb, and then the `hidraw` node.
It needs access to both.
For the own ID, install `etc/70-openmupp-cmsis-dap.rules` (USB device node) and `etc/70-openmupp-avr.rules` (`hidraw`).
For the medbg build, use the udev rules of Bloom (`99-bloom.rules`).
Alternatively, copy the `SUBSYSTEM=="usb"` line of `etc/70-openmupp-cmsis-dap.rules` and change the ID to `03eb`/`2145`.
The file `etc/70-openmupp-avr.rules` has a `hidraw` line for the medbg ID.

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
Thus the firmware keeps the target pins, and the AVR commands do not start on this board today (see [OpenMUPP v1 probe board](boards.md#openmupp-v1-probe-board)).
The adapter that maps the target connector J4 to the ICSP pins is part of the board design.
See the [board repository](https://github.com/OpenMUPP/openmupp-v1) for the pin map of J4.

## avrdude commands

The file `etc/avrdude-openmupp.conf` has a programmer entry with the name `openmupp`.
Read it with `-C +etc/avrdude-openmupp.conf`. Use avrdude version 7.1 or later.
Run the commands in the top directory of the repository, or give the full path of the file.
The entry is not part of avrdude.
The examples are for ISP and a target clock of 8 MHz or more. For a slower target, change the `-B` value (see [Low-clock targets](#low-clock-targets)).
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

## debugWIRE

debugWIRE uses only the RESET pin of the target. It is available on the small megaAVR (for example ATmega328P) and the tinyAVR.
The DWEN fuse enables it. When DWEN is programmed, ISP does not work.

### Wiring

On the YD-CH32V307VCT6 board, connect PB12 (drive, P3-46) and PB11 (read-back, P2-42) to the RESET pin of the target.
Keep the ISP wires connected, because ISP sets and clears the DWEN fuse.
The target needs its pull-up on RESET (10 kOhm or more). Remove a capacitor on RESET.
Connect the target GND to the probe GND. Only 3.3 V targets.
The target clock must be 282 kHz or more (see [boards.md](boards.md)).

### Enable debugWIRE

1. Program the DWEN fuse over ISP. For an ATmega328P, DWEN is bit 6 of the high fuse (0 = programmed).
   Example for the default high fuse 0xD9: `avrdude -C +etc/avrdude-openmupp.conf -c openmupp -p m328p -P usb -B 125kHz -U hfuse:w:0x99:m`.
2. Switch the target power off and on.

### avrdude over debugWIRE

The entry `openmupp_dw` of `etc/avrdude-openmupp.conf` has the type `jtagice3_dw`, as `xplainedmini_dw` of avrdude.
debugWIRE has no chip erase. Use `-D`: the probe erases each flash page before it writes it.
avrdude cannot read or write the fuses and lock bits over debugWIRE.

```sh
# Read the signature
avrdude -C +etc/avrdude-openmupp.conf -c openmupp_dw -p m328p -P usb

# Write and verify the flash
avrdude -C +etc/avrdude-openmupp.conf -c openmupp_dw -p m328p -P usb -D -U flash:w:firmware.hex:i
```

At the end of the session, the probe resets the target, and the program starts.

### Back to ISP

The probe command "disable debugWIRE" turns dW off until the next power cycle.
Bloom sends it at the end of the session when `disable_debug_wire_pre_disconnect` is `true`.
Then program the DWEN fuse back to 1 over ISP, without a power cycle in between.

avrdude does both steps itself. When ISP fails on a chip with DWEN, avrdude sends "disable debugWIRE" and then uses ISP:
`avrdude -C +etc/avrdude-openmupp.conf -c openmupp -p t85 -P usb -B 125kHz -U hfuse:w:0xDF:m`.
For this step, avrdude sends no device parameters. The probe then accepts only "disable debugWIRE" and the end of the session.

- After the disable, Bloom 2.1 logs `AVR8 Detach command failed - Failure reason: Physical not activated`. dW is off and ISP works. Only the message is wrong.

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

### debugWIRE hardware test

The test used a YD-CH32V307VCT6 board and a Digispark (ATtiny85) with a 3.3 V supply on its 5V pin.
The target had a 10 kOhm pull-up from RESET to VCC.
The tools were avrdude 8.3, Bloom 2.1.0 (commit `3e9ddfa2`) and avr-gdb 17.2.
The target clock was the internal 8 MHz RC oscillator (low fuse `0xE2`) and the same oscillator with CKDIV8, 1 MHz (low fuse `0x62`).
At 8 MHz, the measured dW baud rate was 51.6 kbaud in some sessions and 61.6 kbaud in others. The cause is not known.

| Test | 8 MHz | 1 MHz |
|---|---|---|
| ISP: program DWEN, then a power cycle | Pass | Pass |
| avrdude `openmupp_dw`: sign-on and signature | Pass | Pass |
| avrdude `openmupp_dw`: write and verify 1656 bytes (26 pages) with `-D`, then write a different image and verify it again in a new session | Pass. The write takes 4.6 s. | Pass. The write takes 34.7 s. |
| avr-gdb `load` and `compare-sections` through Bloom | Pass | Pass |
| A breakpoint on `main` and on a line in the loop, `continue` | Pass | Pass |
| `next` and `stepi` | Pass | Pass |
| Read and write a variable in SRAM, `display` at each stop | Pass | Pass |
| Interrupt of a running target (Ctrl-C in avr-gdb) | Pass | Not tested |
| "disable debugWIRE" from Bloom, then ISP without a power cycle: clear DWEN | Pass | Pass |

Bloom shows `Available hardware breakpoints: 0`, so it uses software breakpoints.
The probe writes one flash page to insert or remove a software breakpoint. A run or step from a software breakpoint writes two pages (see [cmsis-dap.md](cmsis-dap.md#avr-commands)).
A gdb `watch` on SRAM is not tested. dW has no data breakpoint, so avr-gdb steps each instruction.

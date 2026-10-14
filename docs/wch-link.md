# WCH-Link personality

The `wch-link` personality makes the dongle a WCH-LinkE in RISC-V mode.
The dongle then debugs and programs WCH RISC-V chips (CH32V) with the host tools `wlink` and `minichlink`.
The personality speaks the USB protocol of the WCH-LinkE.
[wch-link-protocol.md](wch-link-protocol.md) describes the protocol, the wire frames and the command processor.

## Build

Use the preset of the board with the `-wch-link` suffix, for example `mini-wch-link`:

```sh
cmake --preset mini-wch-link
cmake --build --preset mini-wch-link -j
```

Or set the CMake option `VSL_PROTOCOL` to `wch-link`.
All boards build with this value.
The build writes `versaloon-ng-<board>-wch-link-<image>-<type>-<version>.hex` and `.bin`.
Write the image as in [Firmware update](firmware-update.md).

The `wch-link` image has these properties:

- USB interface 0 is the WCH-Link interface: bulk OUT `0x01` and IN `0x81` for commands, bulk OUT `0x02` and IN `0x82` for data.
- The image has no CDC-ACM serial port. The packet memory of the USB peripheral (512 bytes) has no room for it.
  Thus the UART bridge and the debug log are off.
  A `Debug` build of this image needs `-DVSL_DEBUG_CDC=OFF`, and it has no debug log. CMake stops with an error when `VSL_DEBUG_CDC` is on.
- The image does not link the Versaloon and CMSIS-DAP code. The SWD and JTAG engines of these personalities are not used.
- The LEDs show the target power, the command activity and the USB state. They show no probe error pattern.
- The image has no `VERSALOON_FW_UPDATE` command. Use the key to start the bootloader (see [Firmware update](firmware-update.md)).
- There is no dual image with this personality. The key does not switch to it.

## USB ID

The default USB ID is `1209:0001`, the test ID of pid.codes.
It stays until the project has its own product ID.
The product string is `Versaloon WCH-Link`.

`wlink` and `minichlink` look for a probe with the ID `1a86:8010` of WCH.
Thus they do not find a probe with the default ID.
The CMake option `VSL_WCH_LINK_WCH_USB_ID` builds the image with the ID `1a86:8010`:

```sh
cmake --preset mini-wch-link-wch-id
cmake --build --preset mini-wch-link-wch-id -j
```

The vendor ID `1a86` belongs to WCH.
Use this option only for personal use.
Do not publish an image with this ID.
The names of these image files have `-wch-link-wch-id` after the board.
The CI job `build-mini-wch-link-wch-id` builds this image to check the build, and keeps no artifact.
The option is only valid with `VSL_PROTOCOL=wch-link`.

## Connect a target

The dongle uses the SWD pins of the board (see [Pins and USB ID](wch-link-protocol.md#pins-and-usb-id) for the pin of each board):

| Target | Wires | Dongle signals |
|---|---|---|
| CH32V003 | One wire: SWIO on PD1 | SWDIO |
| CH32V20x | Two wires: SWDIO on PA13, SWCLK on PA14 | SWDIO and SWCLK |

Connect the ground of the dongle to the ground of the target.
The dongle does not supply the target. The WCH-Link commands for the target supply are not supported.
Power the target from its own supply.

A connect needs a known chip ID.
The firmware knows the CH32V003, CH32V203, CH32V207 and CH32V208.
A CH32V203 and a CH32V207 are not captured (see [Connect](wch-link-protocol.md#connect)).
For another chip, the connect fails.

## Use the probe

On Linux, a udev rule gives a normal user access to the probe.
Copy `etc/70-versaloon-wch-link.rules` to `/etc/udev/rules.d/`.
Then reload the rules with `udevadm control --reload` and plug in the probe again.
The file has a rule for the default ID and a rule for the ID of WCH.
The file number must stay below 73, because systemd applies the `uaccess` tag in `73-seat-late.rules`.
The rule for the default ID also checks the product string, because other devices use the test ID too.

```text
SUBSYSTEM=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{product}=="Versaloon WCH-Link", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="8010", MODE="0660", TAG+="uaccess"
```

The host tools then find the probe if it has the ID that they look for (see [USB ID](#usb-id)).
Examples with `wlink`:

```sh
wlink status
wlink flash firmware.bin
wlink erase
wlink dump 0x08000000 256
```

`minichlink` writes and reads the flash of the target too.
The commands that the personality supports are in [Commands](wch-link-protocol.md#commands).

## Size

The size of the `standalone` image (Release build):

| Board | Size | Limit |
|---|---|---|
| Mini | 24,884 bytes | 65,536 bytes |
| OtherUse | 24,872 bytes | 65,536 bytes |
| ST-Link/V2 | 24,628 bytes | 65,536 bytes |

The `app-8k` image has the limit 57,344 bytes, and the `app-10k` image has the limit 55,296 bytes (see [Flash layout](firmware-update.md#flash-layout-and-size-limit)).
Each image uses less than half of its limit.
CI checks each image against its limit (`ci/check-size.sh`).

For comparison, the Versaloon image of the Mini has 31,812 bytes, the CMSIS-DAP image 35,328 bytes, and the dual image 41,488 bytes.

### Combined image

The question was whether an image with all three personalities fits the Mini.
The Mini has an STM32F103CB with 128 KB flash.
The firmware and the linker script use only the first 64 KB, so the limits above apply.

The dual image of the Mini (Versaloon and CMSIS-DAP) has 41,488 bytes.
The code that only the WCH-Link personality uses has about 6.1 KB (the symbols of the processor, the flash module, the target adapter, the debug module layer and the USB function).
A combined image would have about 48 KB.
This is under the dual limits: 56,320 bytes for `app-8k` and 54,272 bytes for `app-10k`.
The RAM also fits: the combined image adds about 0.5 KB to the 15.9 KB of data and BSS of the dual image, and the RAM has 19.75 KB.

This is an estimate. No image was built, because the dual code supports two personalities only:
a third personality needs its own descriptors set, a switch code and a change of the key cycle.
The WCH-Link personality has no serial port, but the other two have one. Thus a combined image needs a USB stack that changes its function list.
Thus a combined image would fit in the first 64 KB, and it does not need the other 64 KB of flash.
The combined image is not part of the epic.

## Hardware test

Work item #71 tests the personality on real hardware.
The list of items is in [Hardware test](wch-link-protocol.md#hardware-test).
No hardware test has run on this build yet.

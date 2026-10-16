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

Or set the CMake option `VSL_PERSONALITIES` to `wch-link`.
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
- A multi image can have this personality with others, for example `-DVSL_PERSONALITIES=versaloon,cmsis-dap,wch-link` (see [Multi image](usb-device.md#multi-image)).
  The switch commands of the other personalities (code `2`), the switch request on EP0 and the key switch to it.
  The WCH-Link protocol has no command that switches the personality.
  To leave the WCH-Link personality, use the switch request on EP0 or the key (see [Switch request on EP0](usb-device.md#switch-request-on-ep0)).
  The `wch-link` image has only one personality, so it stalls the switch request on EP0.
  While the WCH-Link personality is active, the image has no serial port, and the debug log of a `Debug` build is silent.

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
In a multi image, the tag ends with `-wch-id`, for example `-vsl+dap+wch-wch-id`.
CI does not build this image.
The option is only valid when `VSL_PERSONALITIES` has `wch-link`.

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
| Mini | 27,124 bytes | 65,536 bytes |
| OtherUse | 27,112 bytes | 65,536 bytes |
| ST-Link/V2 | 26,672 bytes | 65,536 bytes |

The `app-8k` image has the limit 57,344 bytes, and the `app-10k` image has the limit 55,296 bytes (see [Flash layout](firmware-update.md#flash-layout-and-size-limit)).
Each image uses less than half of its limit.
CI checks each image against its limit (`ci/check-size.sh`).

For comparison, the Versaloon image of the Mini has 31,812 bytes, the CMSIS-DAP image 35,328 bytes, and the dual image 41,488 bytes.

### Combined image

The question was whether an image with all three personalities fits the Mini.
The Mini has an STM32F103CB with 128 KB flash.
The firmware and the linker script use only the first 64 KB, so the limits above apply.

A build measured the size. The build is a test build only. It links the dual image of the Mini (Versaloon and CMSIS-DAP) and the objects of the WCH-Link personality.
The measurement is from before the one-wire timer. The timer adds about 2.2 KB, so each margin below is about 2.2 KB smaller.
It has no third descriptor set and no switch code.

| Image (Mini) | Size | Limit |
|---|---|---|
| Dual image | 41,488 bytes | 64,512 bytes |
| Dual image with the WCH-Link objects | 51,312 bytes | 64,512 bytes |
| Same, `app-8k` | 51,304 bytes | 56,320 bytes |
| Same, `app-10k` | 51,312 bytes | 54,272 bytes |

The WCH-Link code adds 9.8 KB.
The wire engine and the debug module layer are templates, and they are a large part of this size.
The `app-10k` image has a margin of 2,960 bytes only.
A third descriptor set and the switch code use some of this margin.
The data and BSS grow from 15.9 KB to 16.4 KB. The RAM has 19.75 KB.

Thus a combined image fits in the first 64 KB, but the margin for the legacy bootloader is small.
It does not need the other 64 KB of flash.

The firmware now builds the combined image: `-DVSL_PERSONALITIES=versaloon,cmsis-dap,wch-link` (see [Multi image](usb-device.md#multi-image)).
A Release build of the Mini measured these sizes (`.bin` files, with the one-wire timer and the switch request on EP0):

| Image (Mini, Release) | Size | Limit |
|---|---|---|
| `standalone` | 56,012 bytes | 64,512 bytes |
| `app-8k` | 56,012 bytes | 56,320 bytes |
| `app-10k` | Does not fit: 1,740 bytes too large | 54,272 bytes |

The data and BSS use 16.5 KB.
The `app-10k` image of the combined image does not link.
The `app-8k` image has a margin of 308 bytes only.
A `Debug` build of the combined image fits no image.

## Hardware test

Work item #71 tests the personality on real hardware.
The list of items is in [Hardware test](wch-link-protocol.md#hardware-test).

Setup:

| Item | Value |
|---|---|
| Probe | OtherUse board, `wch-link` image with `VSL_WCH_LINK_WCH_USB_ID` (USB ID `1a86:8010`) |
| Target | CH32V208WBU6 board, external power. SWDIO on PA13, SWCLK on PA14. |
| Host | Linux, wlink 0.1.2 |
| Logic analyzer | Saleae Logic Pro 8: SWDIO on channel 0, SWCLK on channel 1 |

Results:

| Item | Result |
|---|---|
| USB ID | wlink and minichlink find the probe only with the WCH USB ID. The default ID `1209:0001` hides it (issue #76). |
| Connect on two wires (CH32V208), `wlink status` | Passed, 6 of 6 runs. The connect tries one wire first, then two wires. wlink shows the chip ID `0x2080053c`, 160 KB flash, `misa` `RV32ACIMUX` and `dmstatus` `0x382`. |
| `wlink dump`, `wlink regs` | Passed. A dump of the full flash (160 KB) takes 11.4 s. Two dumps are the same. |
| `wlink halt`, `resume`, `reset` | Passed. |
| `wlink erase` | Passed, 0.16 s. The flash then reads `0xe339e339`. |
| `wlink flash` | Passed. 147 KB take 23 s. A dump after the program is the same as the image. |
| `wlink protect` | Passed. The protection starts at the next reset, as on the WCH-LinkE. A dump then reads `0xffffffff`. |
| `wlink unprotect` | Passed. The protection ends at once, without a power cycle: the `ndmreset` loads `RDPR`. The chip erases the flash. The option bytes then read `RDPR` `0xa5`, `USER` `0x3f`, `DATA0` and `DATA1` `0x00`, `WRPR` `0xff`. |
| minichlink | Failed first: minichlink sends the chip type `09` also for a CH32V208, and the processor tried only one wire for this type. The processor now tries both wires for all types. |
| `minichlink -i`, `-r`, `-w` | Passed after the fix. A read of 160 KB takes 6.9 s, and it is the same as the wlink dump. A write of 147 KB takes 22 s. A wlink dump after the write is the same as the image. |
| wlink after `minichlink -w` | Failed first: each memory read gave the error frame `81 55 01 01` until a USB replug. minichlink does not read the block acknowledge of the last block, and the acknowledge stayed on the data IN endpoint (issue #77). The WCH-LinkE has the same problem in part: the first wlink dump fails with `Invalid payload length`, and the next one works. The personality now takes the packet back. Passed after the fix, 3 of 3 runs: the first wlink dump gives the right data. |

The first connect failed: wlink got the error frame `81 55 01 01`.
The cause was the handshake bits of the two-wire frame: the probe must drive bit 13 low and bit 51 high (see [Wire frame format](wch-link-protocol.md#wire-frame-format)).
The engine released these bits, and the CH32V208 then dropped each frame after bit 11.

### CH32V003 (one wire)

Setup: the same probe and host. The target is a CH32V003F4P6 board with its own supply, SWIO (PD1) on SWDIO. The logic analyzer has SWDIO on channel 1 and SWCLK on channel 3.

The first connect failed. The probe sent the right bits, but with wrong low pulses: a 1 was 120 to 530 ns, a 0 was 460 to 1060 ns (WCH-LinkE: 250 and 860 ns).
The engine timed each pulse with waits on the cycle counter. The time of the code between the waits changed the pulses, and the CH32V003 did not answer.
The engine now makes the bits with a timer and DMA (see [One-wire timer](wch-link-protocol.md#one-wire-timer)).
On the wire, the low pulses are now 250 to 260 ns and 860 to 870 ns, and the high level is 370 to 380 ns.

Results with the timer:

| Item | Result |
|---|---|
| `wlink status` | Passed, 6 of 6 runs. wlink shows the chip ID `0x00300500`, 16 KB flash, `misa` `RV32CEX`, `marchid` `WCH-V2A` and `dmstatus` `0x4c0382`. |
| `wlink dump`, `wlink regs` | Passed. A dump of the full flash (16 KB) takes 1.7 s. Two dumps are the same. |
| `wlink halt`, `resume`, `reset` | Passed. |
| `wlink erase` | Passed. The flash then reads `0xff`. |
| `wlink flash` | Passed. 16 KB take 4.4 s (the full wlink run). A dump after the program is the same as the image. The WCH-LinkE takes 3.5 s. A read bit of the personality has a fixed time of 1.3 µs, so the personality is slower. |
| `wlink protect` | Passed. The protection starts at the next reset, as on the WCH-LinkE. |
| `wlink unprotect` | Passed. The protection ends at once, and the chip erases the flash. The 16-bit write of the option bytes works: they then read `RDPR` `0xa5`, `USER` `0x17`, `DATA0` `0x00`, `DATA1` `0xff`, `WRPR0` and `WRPR1` `0xff`. |
| `minichlink -i`, `-w` | Passed. A write of 16 KB takes 5.2 s. Three wlink dumps after the write are the same as the image (issue #77 does not occur). |
| `minichlink -r` | Passed for a read of 16,380 bytes (0.9 s), and it is the same as the wlink dump. A read that ends at `0x08004000` (the end of the flash) fails with `cmderr` 3 (exception). The code before the timer gave the same result, so the probe is not the cause. minichlink seems to read one word after the end of the read. A check with a WCH-LinkE is open. |

### CH32V208 with the timer image

The two-wire engine did not change, but the probe now sets up TIM3 and DMA at each connect, and the connect tries one wire first with the timer.
A second OtherUse probe (legacy bootloader, `app-10k` image) ran the CH32V208 items again:

| Item | Result |
|---|---|
| `wlink status` | Passed, 20 of 20 runs. |
| `wlink dump` | Passed. Three dumps of the full flash take 11.4 s each, and they are the same as a dump with the image before the timer. |
| `wlink halt`, `regs`, `resume`, `reset` | Passed. |
| `wlink erase`, `wlink flash` | Passed. The erase takes 0.16 s. A program of 160 KB takes 25.5 s, and a dump after it is the same as the image. |
| `wlink protect`, `wlink unprotect` | Passed. The option bytes after the unprotect are the same as in the first test. |
| `minichlink -r`, `-w` | Passed. A read of 160 KB takes 6.9 s. A write of 160 KB takes 24.3 s. Three wlink dumps after the write are the same as the image. |

First, the memory reads failed at random points with both images (with and without the timer).
The cause was the wiring between the probe and the target: after a new wiring, all items passed.
The first probe also gave more failures than the second probe. Its wiring is not checked.

Open items:

- The minichlink read of the full CH32V003 flash with a WCH-LinkE.
- The WCH OpenOCD fork (if time allows).

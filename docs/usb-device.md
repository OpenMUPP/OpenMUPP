# USB device

The dongle is a USB full-speed composite device with the USB identity of the original Versaloon firmware.
Thus unmodified OpenOCD (`vsllink` driver) finds it.
`src/usb/UsbDescriptors.h` has the descriptors.

| Item | Value |
|---|---|
| VID:PID | `0x0483:0xA038` (the OpenOCD default) |
| Device class | `0xEF/0x02/0x01` (miscellaneous, with interface association descriptors) |
| Manufacturer string | `OpenMUPP` |
| Product string | `Versaloon`. OpenOCD looks for this word. |
| Serial number string | The 96-bit chip UID in hexadecimal, 24 digits |
| Maximum power | 200 mA, bus-powered |

| Interface | Class | Endpoints | Function |
|---|---|---|---|
| 0 | Vendor-specific (`0xFF`) | bulk OUT `0x03`, bulk IN `0x82`, 64 bytes | Versaloon protocol (OpenOCD) |
| 1 | CDC (`0x02`), ACM | interrupt IN `0x81`, 16 bytes | Serial port, control interface |
| 2 | CDC data (`0x0A`) | bulk OUT `0x04`, bulk IN `0x84`, 64 bytes | Serial port, data interface |

An interface association descriptor (IAD) groups the interfaces 1 and 2.
The Call Management descriptor points to data interface 2.
The original firmware has the wrong value 1 there.
There is one serial port only.

`src/usb/UsbStack.cpp` runs the libArT USB device stack on the STM32F1 USB peripheral.
The USB interrupt only puts the events into a queue.
The main loop runs the USB protocol.
At the end of each iteration, the main loop sleeps (`WFI`) until the next interrupt. On the CH32V307 the interrupts stay enabled during the `WFI`, because the QingKe V4F does not end a `WFI` for a pending interrupt while `MIE` is 0. A rare race can then add up to one SysTick (1 ms) of latency.
It does not sleep while USB events wait in the queue (`UsbStack::hasPendingEvents()`).
An event that arrives after the USB poll has no pending interrupt to stop the sleep.
For example, the double-buffered OUT endpoint reports a waiting packet from the main loop when the endpoint is armed again.
Without this check, such an event waits for the next SysTick interrupt, up to 1 ms.
The main loop also does not sleep while a debugger is connected (`DHCSR.C_DEBUGEN` is 1).
On the STM32H5, the debugger reads zeros from flash, SRAM and the peripherals while the core sleeps.
The board definition supplies the USB D+ pull-up pin.
The device connects when `main()` starts the USB device.
A board without a pull-up pin (the ST-Link/V2 mini clone) has a fixed pull-up.
On such an STM32F1 board, the firmware sets D+ (PA12) low for 20 ms before it starts the USB device.
Thus the host sees a disconnect also after a reset by a debugger or a watchdog.
Without this step, the host keeps the device of the old firmware and does not enumerate the new one.

The Versaloon OUT endpoint `0x03` is double-buffered (system-stm32 `hw::UsbDevice`, "Double-buffered bulk OUT").
The hardware receives the next packet of a command while the main loop handles the last packet.
Thus the host gets fewer NAKs, and a large command arrives faster.
The 512-byte packet memory of the STM32F1 has no room for a double-buffered IN endpoint too.
The buffer descriptor table covers only the endpoint numbers 0 to 4, so it takes 40 bytes instead of 64 bytes.

Interface 0 carries the Versaloon command protocol (see [Versaloon commands](#versaloon-commands)).

The serial port is the [UART bridge](uart-bridge.md).
In a debug build with `MUPP_DEBUG_CDC`, the serial port carries the debug log instead (see [Debug log](debug-log.md)).

## CMSIS-DAP personality

The firmware has a second USB personality: CMSIS-DAP v2 instead of the Versaloon protocol.
Only one personality is active at a time.
The USB peripheral has 512 bytes of packet memory.
The Versaloon interface and a CMSIS-DAP interface do not fit together.
The build option `MUPP_PERSONALITIES` selects the personalities (see [Select the personality](#select-the-personality)).
A multi image has more than one personality, and selects one at each start (see [Multi image](#multi-image)).
The CMSIS-DAP personality is the default.

`src/usb/CmsisDapDescriptors.h` has the descriptors.
`src/usb/CmsisDapFunction.h` has the USB function.

| Item | Value |
|---|---|
| VID:PID | `0x1209:0x0001` |
| Product string | `OpenMUPP CMSIS-DAP` |
| Interface 0 string | `OpenMUPP CMSIS-DAP v2` |
| `bcdUSB` | `0x0201` (USB 2.0 with a BOS descriptor) |
| Other items | As in the Versaloon personality |

The host tools find a CMSIS-DAP v2 interface by the vendor class and the text `CMSIS-DAP` in the interface string.
Interface 0 is vendor-specific (`0xFF`) with bulk OUT `0x03` and bulk IN `0x82`, 64 bytes each.
The interfaces 1 and 2 are the CDC-ACM serial port, as in the Versaloon personality.
The endpoint numbers and sizes are the same as in the Versaloon personality.
Thus the packet memory use does not change from the Versaloon personality.
The endpoints and EP0 use 464 bytes, with a 64-byte buffer descriptor table.
The firmware uses a 40-byte table and a double-buffered OUT endpoint `0x03`: 504 bytes.

`usb::CmsisDapFunction` sends each OUT packet to the [CMSIS-DAP command processor](cmsis-dap.md).
It sends each response as one IN packet.
A response of 64 bytes has no zero-length packet.
The processor holds up to 4 packets.
When all 4 slots are in use, the OUT endpoint NAKs until the host reads a response.
`DAP_Info` reports the packet size 64 and the packet count 4.
When a bus reset or `SET_CONFIGURATION 0` closes the interface, the function calls `Processor::hostGone()`.
A suspend only releases the target pins, because the USB transfer state stays.
When a `DAP_QueueCommands` packet takes the last free slot, the processor runs the queue at once.
Thus queued packets alone do not hold the OUT endpoint, and each queued packet gets its response.

### MS OS 2.0 descriptors

Windows 8.1 and later bind WinUSB to interface 0 of the CMSIS-DAP personality with no driver installation.
The serial port keeps the CDC driver of Windows (`usbser.sys`), which binds with no setup file from Windows 10.
`src/usb/CmsisDapDescriptors.h` has these descriptors, built with `usb/MsOs20.h` of libArT:

1. The device descriptor has `bcdUSB` `0x0201`. Thus Windows reads the BOS descriptor.
2. The BOS descriptor has one device capability: the MS OS 2.0 platform capability.
   It gives the vendor code `0x20` and the size of the descriptor set (170 bytes).
3. Windows reads the descriptor set with a vendor request: `bmRequestType` `0xC0`, `bRequest` `0x20`, `wIndex` `7`.
   `UsbStack` answers it with `msos20::DescriptorSetHandler`, in the Address and Configured states.
4. The set has one function subset, for interface 0.
   It has the compatible ID `WINUSB` and the device interface GUID `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`.
   This is the GUID of the Arm CMSIS-DAP and DAPLink firmware.
   The device has one configuration, so the set has no configuration subset.

The Versaloon personality has `bcdUSB` `0x0200` and no BOS. It still needs a WinUSB driver from Zadig.

A driver that was installed before for the probe, for example with Zadig, has priority over the WinUSB compatible ID.
Thus such a probe keeps its old driver.
Uninstall the device and its driver in the Device Manager (see [Use the probe](cmsis-dap.md#use-the-probe)).
Windows also keeps data about the descriptors of a device in the registry key `HKLM\SYSTEM\CurrentControlSet\Control\usbflags\12090001rrrr`.
`rrrr` is the `bcdDevice` in hexadecimal.
If the probe does not get WinUSB after that, delete this key and plug in the probe again.

### Select the personality

The CMake option `MUPP_PERSONALITIES` selects the personalities of the image.
It is a list of one or more of these personalities, with `;` or `,` between them:

| Personality | Code | USB interface 0 | VID:PID | Sources |
|---|---|---|---|---|
| `versaloon` | `0` | Versaloon vendor interface (`usb::VersaloonFunction`) | `0483:A038` | `usb/VersaloonFunction.cpp`, `protocol/versaloon/`, `boot/Bootloader.cpp` |
| `cmsis-dap` (default) | `1` | CMSIS-DAP v2 (`usb::CmsisDapFunction`) | `1209:0001` | `protocol/cmsisdap/` |
| `wch-link` | `2` | WCH-Link (`usb::WchLinkFunction`), no serial port | `1209:0001`, or `1A86:8010` with `MUPP_WCH_LINK_WCH_USB_ID` | `protocol/wchlink/`, `rvswd/` (see [WCH-Link personality](wch-link.md)) |

For example, `-DMUPP_PERSONALITIES=versaloon,cmsis-dap,wch-link` builds an image with all three personalities.
The order in the list does not matter.
An image with more than one personality is a multi image (see [Multi image](#multi-image)).
It also has `personality/` and `board/SettingsFlashPage.cpp`.
`MUPP_DEFAULT_PERSONALITY` selects the personality of a multi image when no personality is stored.
`cmake/Personalities.cmake` has the table of the personalities (see [Add a personality](cmake.md#add-a-personality)).

The option defines `MUPP_WITH_VERSALOON`, `MUPP_WITH_CMSIS_DAP` or `MUPP_WITH_WCH_LINK` for each personality in the image.
A multi image also gets `MUPP_MULTI_PERSONALITY` and `MUPP_DEFAULT_PERSONALITY=<code>`.
`MUPP_MULTI_PERSONALITY` marks an image with more than one personality.
`MUPP_DEFAULT_PERSONALITY` is the code of the personality when no personality is stored.
`personality::all` in `src/personality/Personality.h` is the list of the personalities in the image, in code order.
The header makes the list from the `MUPP_WITH_*` defines.
`usb::UsbStack` and `main.cpp` use the defines to select the USB function, the descriptors and the protocol objects.
The CMake file also removes the source files of the personalities that are not in the image.
Thus the CMSIS-DAP image does not link the Versaloon command and reply buffers (2 x 4096 bytes).
The Versaloon and the CMSIS-DAP personalities share the CDC-ACM serial port, the UART bridge and the debug log. All personalities share the LEDs.
The WCH-Link personality has two bulk endpoint pairs and no room in the packet memory for the CDC-ACM endpoints, so it has no serial port.
In a multi image, the serial port is not there while the WCH-Link personality is active.

The CMSIS-DAP image differs from the Versaloon image in these points:

- It has no `VERSALOON_FW_UPDATE` command.
  Start the bootloader with `ID_DAP_Vendor0`, the switch request on EP0 or the key (see [Firmware update](firmware-update.md#bootloader-code)).
- It has no `GET_TVCC`. `ID_DAP_Vendor2` switches the target power and reads the target voltage (see [Target power](cmsis-dap.md#target-power)).
- JTAG uses the CMSIS-DAP JTAG commands instead of the `JTAG_RAW` block (see [JTAG](cmsis-dap.md#jtag)).

The names of the image files have the text `-cmsis-dap` after the board name.

### Multi image

A multi image has more than one personality, for example `MUPP_PERSONALITIES=versaloon,cmsis-dap`.
A multi image with the Versaloon and the CMSIS-DAP personalities is also called a dual image.
The `-dual` presets build the multi image with the CMSIS-DAP and the WCH-Link personalities.
One personality is active after each start.
The active personality has its descriptors, its USB function and its commands, as in the image with one personality.
The firmware changes the personality only with a reset.

The firmware stores the personality in the last 1 KB flash page of the application area, at `0x0800FC00`.
On the WeAct STM32H523 core board (STM32H5, STM32H523CC, 256 KB flash), the page is the last 8 KB flash sector of bank 2, at `0x0803E000`.
The linker script keeps this page out of the image.
Thus a multi image and an image with the RISC-V bridge have 1 KB (STM32F103) or 8 KB (STM32H5) less flash than the other images (see [Flash layout and size limit](firmware-update.md#flash-layout-and-size-limit)).
When the page holds no personality, the image uses `MUPP_DEFAULT_PERSONALITY`.
The default is `cmsis-dap` when the image has it, else the first personality of the image in code order.
`src/personality/PersonalityStore.h` has the page format.
The record also holds the stored mode of the RISC-V bridge (see [Stored mode](cmsis-dap.md#stored-mode)).
An image with the bridge has the page also with one personality.
The page is a log of 16-bit records, so the firmware erases the page only one time in 512 switches.
The STM32H5 writes the flash in 128-bit flash words with ECC (error correction code), and each flash word can be written only one time.
Thus each record uses one flash word, and the sector also holds 512 records.
`src/personality/FlashWordSlot.h` has the layout of a flash word.
A power loss during a write can leave a flash word with a bad ECC.
A read of that flash word causes an NMI (non-maskable interrupt).
The NMI handler of the firmware then marks the record as not valid, and the image starts with its default personality.
The firmware also marks a record as not valid when the ECC corrected one bit of it.

To change the personality, use one of these methods:

- In the Versaloon personality, send `SWITCH_PERSONALITY`: `[0x0E][personality u8]` (see [Versaloon commands](#versaloon-commands)).
- In the CMSIS-DAP personality, send `ID_DAP_Vendor0`: `[0x80][personality u8]` (see [Commands](cmsis-dap.md#commands)). The status of a refused switch is `0xFF`.
- In each personality, send the switch request on EP0 (see [Switch request on EP0](#switch-request-on-ep0)).
  A multi image with WCH-Link needs this request or the key to leave WCH-Link, because the WCH-Link protocol has no free command for a switch.
- Hold the key down for 3 s while the firmware runs. All LEDs flash three times when the 3 s end, and then stay on. Release the key. The firmware then selects the next personality in the list. With two personalities, this is the other one.
  The key also works in the WCH-Link personality.
  The ST-Link/V2-1 has no key: use the switch request on EP0 to leave the WCH-Link personality.

The personality code is `0` for Versaloon, `1` for CMSIS-DAP and `2` for WCH-Link.
The firmware refuses a switch to a code that is not in the image.
The key cycle uses the code order and skips the personalities that are not in the image.
The settings page has room for four personality codes (`0` to `3`).
A new personality gets the next code, one entry in `personality::everyPersonality` and one `MUPP_WITH_*` define in `personality::has()`.
`tools/mupp_probe.py` sends the switch from the host.
It needs the `pyusb` package:

```sh
python3 tools/mupp_probe.py show
python3 tools/mupp_probe.py switch cmsis-dap
python3 tools/mupp_probe.py switch wch-link
python3 tools/mupp_probe.py switch versaloon
python3 tools/mupp_probe.py switch bootloader
```

`switch bootloader` starts `openmupp-bootloader` (see [Bootloader code](firmware-update.md#bootloader-code)).

The tool finds the dongle by the VID:PID of each personality.
`--id VID:PID` selects the dongle by this ID instead, without the product string check.
This also works for a dongle with a custom ID.
The tool takes the personality from the ID when it knows the ID.
For an unknown ID, and for the shared ID `1209:0001` (CMSIS-DAP and WCH-Link), give it with `--active versaloon|cmsis-dap|wch-link`.
The tool still needs access to the device, so `--id` does not replace the udev rules.
Without `--active`, the switch request on EP0 still works, but the fallback command for old firmware does not.
After a switch, the tool waits for the target personality at its standard IDs.
For the ID `1209:0001`, it also compares the product string with `OpenMUPP CMSIS-DAP` or `OpenMUPP WCH-Link` (`Versaloon WCH-Link` for older firmware).
Other devices use the IDs `1209:0001` and `1a86:8010` too.
The tool ignores a device with one of these IDs and a different product string.
With the ID `1a86:8010`, the tool also accepts the product string `WCH-Link` of the WCH ID build (see [USB ID](wch-link.md#usb-id)).
The real WCH-LinkE has the same product string, so the tool then also compares the manufacturer string with `OpenMUPP`.
Use `--serial` to select one dongle when more than one is connected.

The tool sends the switch request on EP0 for each switch (see [Switch request on EP0](#switch-request-on-ep0)).
A firmware from before the EP0 request stalls it.
Thus, after a stall, the tool sends `SWITCH_PERSONALITY` or `ID_DAP_Vendor0` on interface 0.
The reply of this command tells if the firmware refused the switch.
In the WCH-Link personality, the tool has no other command, so a stall means that the firmware refused the switch.
The dongle can disconnect before the status stage ends.
Thus, after a timeout, an I/O error or a lost device, the tool also waits for the new personality.
After all other errors, the tool stops with an error message and does not wait.
After the request, the tool waits 10 s for the device with the VID:PID of the new personality.

After a command, the firmware sends the reply, and then waits 10 ms.
Then the firmware disconnects from USB, writes the flash page and resets.
The host sees a disconnect, and then the device with the other VID:PID.
A switch to the active personality does not reset.
It also cancels a switch that the firmware did not do yet.
If the flash write fails, the firmware starts again with the old personality.

#### Multi image hardware test

`tools/probe_hw_test.py` tests the Versaloon and the CMSIS-DAP personalities of a multi image on an STM32F1 target.
Connect the target on SWD and JTAG, and connect the UART TX pin to the UART RX pin.
The script runs SWD before JTAG with each tool.
On the WeAct STM32H523 core board, all JTAG steps failed without TRST and SRST, and passed with them (see [Hardware test](boards.md#hardware-test)).
The script switches to the Versaloon personality and runs the serial loopback test and OpenOCD with the `vsllink` driver.
It then switches to the CMSIS-DAP personality and runs the serial and `DAP_UART_*` loopback tests, OpenOCD, pyOCD and probe-rs.
Each tool runs on SWD and JTAG, at each speed of `--swd-speeds` and `--jtag-speeds`.
The default speeds are 1000, 2000 and 4000 kHz for SWD, and 1000 and 9000 kHz for JTAG.
In the CMSIS-DAP personality on an STM32F1 probe, only 4000 kHz runs the untimed SWD engine (`swd::maxClockHz`).
The script erases the flash of the target:

```sh
python3 tools/probe_hw_test.py
python3 tools/probe_hw_test.py --full-uart --skip probe-rs
```

OpenOCD writes, verifies and reads back a 32 KiB image.
It then checks reset and halt, two single steps, a hardware breakpoint, RAM write and read, and run and halt.
The pyOCD checks come from `tools/pyocd_jtag_test.py`.
probe-rs writes, verifies and reads back the image, writes and reads RAM, and resets the target.
probe-rs cannot set the target clock, and the target runs at 8 MHz after a reset.
Thus the script skips probe-rs on JTAG above 1333 kHz (8 MHz / 6).
The pyOCD target and the probe-rs chip come from the device ID and the flash size of the target.
The script reads these values in the first OpenOCD run.
A low or medium density chip needs the `Keil.STM32F1xx_DFP` pack in pyOCD.
With `--serial`, all tools use only the dongle with this serial number.

Status: the test passed on 2026-10-16.
Probe: Mini board.
Image: `VSL_PERSONALITIES=versaloon,cmsis-dap,wch-link`, `mini` preset, `Release` build, `v0.9.0-113-gb2dab08` (`dev` with the fix of the SWD rules below).
Target: STM32F103ZE (device ID `0x414`, 512 KiB flash).
Host: Linux, OpenOCD 0.12.0+dev-01355-gcba596a3e, pyOCD and probe-rs. The test did not record the pyOCD and probe-rs versions.
The results come from two runs of the script with this image. The serial loopback passed with `--full-uart` on `v0.9.0-112-ga8a1abe`.

| Check | Versaloon | CMSIS-DAP |
|---|---|---|
| Switch with the switch request on EP0 | pass | pass |
| Serial loopback, quick set (3 baud rates, 8N and 8E, 1 and 2 stop bits) | pass | pass |
| Serial loopback, all line codings (`--full-uart`, `v0.9.0-112-ga8a1abe` only) | pass | pass |
| `DAP_UART_*` loopback | - | pass |
| OpenOCD SWD, 1000 and 2000 kHz | pass | pass |
| OpenOCD JTAG, 1000 and 9000 kHz | pass | pass |
| pyOCD SWD, 1000 and 2000 kHz | - | pass |
| pyOCD JTAG, 1000 and 9000 kHz | - | pass |
| probe-rs SWD, 1000 and 2000 kHz | - | pass |
| probe-rs JTAG, 1000 kHz | - | pass |

Findings:

- With `v0.9.0-112-ga8a1abe`, OpenOCD with the `vsllink` driver failed on SWD in the multi image.
  The first MEM-AP access got the status FAILED (`usbtoswd command 0x28 failed with 0x01`).
  The cause: the CMSIS-DAP processor sets its SWD transfer rules on the shared SWD engine at start, also when the Versaloon personality is active.
  The Versaloon SWD block then ran without idle cycles, and without the ABORT write before a WAIT retry.
  Now INIT and CONFIG of the Versaloon SWD block set the Versaloon rules again.
- The test did not cover the key switch, the old switch commands, the WCH-Link personality and Windows.

#### Switch request on EP0

An image with more than one personality accepts a USB vendor request on the control endpoint (EP0) in each personality.
The request has no data stage:

| Field | Value |
|---|---|
| `bmRequestType` | `0x40`: vendor request, host to device, recipient device |
| `bRequest` | `0x0E` (`usb::switchRequestCode`, the same code as `SWITCH_PERSONALITY`) |
| `wValue` | The personality code, or the bootloader code `0xB0` (see [Bootloader code](firmware-update.md#bootloader-code)) |
| `wIndex` | `0` |
| `wLength` | `0` |

The firmware sends the status stage (ACK), and then waits 10 ms, as for the other commands.
The firmware stalls the request in these conditions:

- The code is not a personality of the image.
- The code is the bootloader code, and the image has no bootloader entry. Only the `app-8k` image and the images of the STM32H5 have one.
- `wIndex` or `wLength` is not `0`, or the direction is device to host.

A stall means that the firmware does not switch.
The firmware accepts the request in the Address and Configured states, so a host can send it before `SET_CONFIGURATION`.
An image with one personality stalls the request for each personality code.
It accepts the bootloader code, when it is an `app-8k` image or an image of the STM32H5.

With libusb, the call is:

```c
libusb_control_transfer(handle, 0x40, 0x0E, code, 0, NULL, 0, 1000);
```

`usb::Device` of libArT has one handler for all vendor requests to the device (`usb::SwitchRequestHandler` in `src/usb/SwitchRequest.h`).
The CMSIS-DAP personality also needs it for the request of the MS OS 2.0 descriptor set (vendor code `0x20`, see [MS OS 2.0 descriptors](#ms-os-20-descriptors)).
Thus the switch request handler sends each other request to the MS OS 2.0 handler.
The switch request code and the MS OS 2.0 vendor code must be different. A `static_assert` in `src/usb/UsbStack.cpp` checks this.

On Windows, a host sends the request through the WinUSB driver of interface 0.
The Versaloon and WCH-Link personalities need a WinUSB or libusb driver on interface 0 for their bulk endpoints, so this driver is always there.
In the CMSIS-DAP personality, the MS OS 2.0 descriptors bind WinUSB to interface 0.
WinUSB must also send a control request to the device recipient.
No test on Windows confirms this yet. The hardware test of the personalities checks it.

The key gesture has these rules:

- The LEDs flash when the key has been down for 3 s.
  A shorter press does nothing.
  The switch itself starts when you release the key, after the flashes.
  The firmware does not reset while the key is down, and waits 50 ms after the release, because a key that is down at reset starts the bootloader.
- The firmware counts the key only after it saw the key released.
  Thus a key that sticks does not change the personality at each start.
- The key at reset starts the bootloader, as before (see [Firmware update](firmware-update.md)).
- The ST-Link/V2-1 has no key. Use the commands.

The Versaloon personality of a multi image has `VERSALOON_FW_UPDATE`, as the Versaloon image.
In each personality, the switch commands with the bootloader code `0xB0` start the bootloader (see [Bootloader code](firmware-update.md#bootloader-code)).

A multi image links the objects of all its personalities.
When the image has the Versaloon personality, the Versaloon command and reply buffers (2 x 4096 bytes) are always in RAM.

## Versaloon commands

The host sends one command in one bulk OUT transfer on endpoint `0x03`.
It reads the reply in one bulk IN transfer on endpoint `0x82`.
All values are little-endian.
`src/protocol/versaloon/` has the protocol code. It does not use the hardware, so the host unit tests check it.

The first byte of a command is the command code:

| Code | Commands | Format |
|---|---|---|
| `0x00`-`0x0F` | Common commands | One packet. |
| `0x20`-`0x7F` | `USB_TO_XXX` commands | `[code][length u16][data]`. `length` is the total size, with the 3-byte header. The command can have many packets. |

The firmware collects the packets until it has `length` bytes (`src/protocol/versaloon/CommandAssembler.h`).
The command buffer and the reply buffer are separate, and each has 4096 bytes.
Thus a reply can be longer than its command.
`GET_INFO` sends the buffer size to the host.
The host sends no command and expects no reply that is larger than this size.

The common commands are:

| Command | Request | Reply |
|---|---|---|
| `GET_INFO` | `[0x00]` | `[buffer size u16][version text][0x00]` |
| `GET_TVCC` | `[0x01]` | `[target voltage in mV u16]`, from the Vref sense (see [POWER block](#power-block)). The value is 0 when the board has no sense or the measurement fails. |
| `GET_HARDWARE` | `[0x02]` | `[hardware ID u8]`, from the board definition (see [Boards](boards.md)). |
| `SWITCH_PERSONALITY` | `[0x0E][personality u8]` | `[status u8]`. The personality is the code of a personality in the image (see [Multi image](#multi-image)), or the bootloader code `0xB0`. Status `0`: the firmware accepts the switch. Status `1`: the firmware refuses it (see below). |
| `FW_UPDATE` | `[0x0F][0xAA]` | `[status u8]` (see [Firmware update](firmware-update.md)). |

The version text has the form `openmupp <version>, board <board>`.
Firmware with the former project name versaloon-ng uses `versaloon-ng` in place of `openmupp`.

`SWITCH_PERSONALITY` changes the USB personality of a multi image (see [Multi image](#multi-image)).
The firmware sends the reply, stores the personality and resets.
An image with one personality refuses the command for each personality code.
The firmware also refuses the code of a personality that is not in the image.
The bootloader code `0xB0` starts the bootloader, also in an image with one personality (see [Bootloader code](firmware-update.md#bootloader-code)).
A switch to the active personality gets status `0`, and the firmware does not reset.
Other common commands have no reply.
The `USB_TO_XXX` commands go to the `USB_TO_ALL` dispatcher (see [USB_TO_ALL](#usb_to_all)).

The reply is one IN transfer.
When the size of the reply is a multiple of 64 bytes, a zero-length packet (ZLP) follows, so that the host read ends.
A reply of 4096 bytes has no ZLP.
The host reads 4096 bytes, so its read ends without a ZLP, and the ZLP would end the next read.
While the reply is on its way, the firmware does not take the next command.
The OUT endpoint can receive one packet of it into its second buffer, and then NAKs.
Thus the next command waits.

The firmware replies with a ZLP to a wrong command.
The host then sees a reply with the wrong size immediately, without a timeout.
A command is wrong in these cases:

- The header is shorter than 3 bytes, or `length` is smaller than 3.
- The transfer has more bytes than `length`.
- A short packet ends the transfer before `length` bytes (a ZLP is a short packet too).
- A common command, or an unused command code, is in a full packet of 64 bytes.
- `length` is larger than the command buffer (overflow).
- The `USB_TO_ALL` framing is malformed, or its reply is larger than the reply buffer (see [USB_TO_ALL](#usb_to_all)).

The ZLP comes at the end of the transfer of the host.
Thus the next transfer starts as a new command.
After an error on a full packet, the firmware discards the packets until a short packet ends the transfer.
If such a transfer ends with a full packet and no ZLP, the firmware also discards the next transfer.
For an overflow, the firmware discards the packets until `length`.

## USB_TO_ALL

OpenOCD sends all adapter operations in `USB_TO_ALL` commands (`0x7F`).
`src/protocol/versaloon/UsbToAll.h` runs them.
A `USB_TO_ALL` command contains blocks, and most blocks contain sub-commands:

```text
command:      [0x7F][total length u16][block][block]...
block:        [type][block length u16][payload]
sub-command:  [command | index][payload length u16][payload]
```

The block length includes the 3-byte block header.
The payload length of a sub-command does not include its header.
The top 5 bits of a sub-command are the command: `INIT` `0x00`, `FINI` `0x08`, `CONFIG` `0x10`, `IN_OUT` `0x28`, `IN` `0x30` or `OUT` `0x38`.
The low 3 bits are the port index.

The reply joins the replies of all sub-commands in order: `[status][data]` for each sub-command.
The host knows the data size of each sub-command.
It rejects the whole reply when the reply size is not correct.
Thus a sub-command that fails also sends its full data size.
The status values are:

| Status | Value |
|---|---|
| `OK` | 0 |
| `FAILED` | 1 |
| `TIMEOUT` | 2 |
| `INVALID_INDEX` | 3 |
| `INVALID_PARA` | 4 |
| `INVALID_CMD` | 5 |
| `CMD_NOT_SUPPORT` | 6 |

The firmware has these blocks:

| Block | Type | Request payload | Reply |
|---|---|---|---|
| `INFO` | `0x7E` | none | `[OK][12-byte capability bitmap]` |
| `DELAY` | `0x61` | `[delay u16]` | `[OK]`. When bit 15 is set, bits 0-14 are the delay in ms. Else the delay is in µs. |
| `GPIO` | `0x23` | Sub-commands, see [GPIO block](#gpio-block) | `[status]`, and `[value u16]` for `IN` |
| `JTAG_RAW` | `0x47` | Sub-commands, see [JTAG_RAW block](#jtag_raw-block) | `[status]`, and `[TDO n bytes]` for `IN_OUT` |
| `SWD` | `0x46` | Sub-commands, see [SWD block](#swd-block) | `[status]`, and data for `IN` and `IN_OUT` |
| `POWER` | `0x60` | Sub-commands, see [POWER block](#power-block) | `[status]`, and `[mV u16]` for `IN` |

Bit n of the capability bitmap is set when the firmware has block `0x20 + n`.
The firmware sets the bits of `GPIO`, `SWD`, `JTAG_RAW`, `POWER`, `INFO`, `DELAY` and `USB_TO_ALL`.
A block that the firmware does not have gets one status byte `CMD_NOT_SUPPORT`.
OpenOCD then stops with an error.

### GPIO block

OpenOCD controls the reset lines SRST and TRST through the `GPIO` block (`src/protocol/versaloon/GpioBlock.h`).
Each bit of a `u16` mask selects one pin of the debug connector:

| Bit | Signal | Mini | ST-Link/V2-1 | ST-Link/V2 mini clone |
|---|---|---|---|---|
| `0x0001` | SRST | PB11 | PB0 | PB6 |
| `0x0002` | TRST | PB10 | none | none |
| `0x0004` | USR1 | none | none | none |
| `0x0008` | USR2 | none | none | none |
| `0x0010` | TCK | PB13 | PA5 | PA5 |
| `0x0020` | TDO (input only) | PB14 | PA6 (not connected) | PA6 (not on the header) |
| `0x0040` | TDI | PB15 | PA7 (not connected) | PA7 (not on the header) |
| `0x0080` | RTCK (input only) | PA8 | none | none |
| `0x0100` | TMS | PB4 out, PB6 in | PB14 out, PB12 in | PB14 out, PB12 in |

The sub-commands use port index 0. The values are `u16`, little-endian:

| Sub-command | Payload | Reply |
|---|---|---|
| `INIT` | none | `[status]` |
| `FINI` | none | `[status]` |
| `CONFIG` | `[mask][io][pull_en][pull]` | `[status]` |
| `OUT` | `[mask][value]` | `[status]` |
| `IN` | `[mask]` | `[status][value]` |

`CONFIG` sets the mode of each pin in `mask`:

| `io` | `pull_en` | `pull` | Pin mode |
|---|---|---|---|
| 1 | 0 | level | Push-pull output at the level |
| 1 | 1 | level | Open-drain output at the level |
| 0 | 0 | any | Floating input |
| 0 | 1 | 1 | Input with pull-up |
| 0 | 1 | 0 | Input with pull-down |

OpenOCD releases SRST as an input with pull-up, and asserts SRST as a push-pull output at the low level.
When the host closes the Versaloon interface, the firmware releases SRST and TRST: they become inputs with pull-up.
The host closes the interface with a USB bus reset, with `SET_CONFIGURATION 0`, or with a USB suspend.
For example, this occurs when OpenOCD stops during `reset halt`, or when the cable is disconnected.
Thus the target does not stay in reset until the next OpenOCD session.
The original firmware kept the reset lines asserted.
`OUT` writes the output level of the pins in `mask`.
On an input with pull, the output register selects the pull direction, as in the original firmware.
`IN` reads the level of the pins in `mask`.
For TMS on the Mini, `IN` reads PB6. A TMS input also makes PB4 an input.

A pin that the board does not have is ignored, and `IN` reads it as 0.
Thus OpenOCD can set TRST on the ST-Link/V2-1.
These requests get `INVALID_PARA`: a bit above `0x0100`, an output on TDO or RTCK, or a wrong payload size.
A port index other than 0 gets `INVALID_INDEX`.

The firmware checks the framing of all blocks and sub-commands before it runs the first one.
When a length in the framing does not fit, the firmware runs nothing and replies with a ZLP.
When the reply does not fit the reply buffer, the firmware stops at the first sub-command that does not fit, and replies with a ZLP.
The sub-commands before it have run.
The host sends no command with a reply that is larger than the buffer size of `GET_INFO`, so this is a host error.

`DELAY` waits in a busy loop, and the main loop stops for this time.
The longest delay is 32.767 s.
The OUT endpoint NAKs during the delay, and the serial port stops.

A top-level command `0x20`-`0x7E` is one block without the `USB_TO_ALL` header, as in the original firmware.

### Serial port during long commands

A command runs in the main loop, so the main loop stops until the command ends.
A long command can take seconds, for example a `JTAG_RAW` shift at 10 kHz or with a slow RTCK, or an `SWD` batch with many WAIT retries.
The RX ring buffer of the [UART bridge](uart-bridge.md) holds only about 89 ms at 115200 baud.

Thus the JTAG and SWD engines call a serial service during the command (`src/protocol/Background.h`).
The call comes at most once per millisecond, at these points:

- `JTAG_RAW`: between two bytes of a shift, or between two SPI chunks of the [fast path](#jtag-fast-path). TCK is at its idle level: low, or high on a board with the fast path.
- `SWD`: before each transfer and each WAIT retry, and between two bytes of a sequence. SWCLK is high.

The serial service does these steps:

1. It delivers the USB events of the serial port endpoints (`UsbStack::pollSerial()`, `src/usb/SerialEventQueue.h`).
   It stops at the first other event: a SETUP packet, a bus reset, a suspend, a resume, or a Versaloon endpoint event.
   That event waits until the command ends, so the order of the events does not change.
   Thus the service does not start a new command, and the host does not disconnect during the command.
   In the CMSIS-DAP personality, the service also delivers the events of the CMSIS-DAP endpoints.
   A CMSIS-DAP packet only waits in the processor, so this starts no command (see [CMSIS-DAP transport](cmsis-dap.md#transport)).
   Thus `DAP_TransferAbort` can stop a running transfer command.
2. It moves the UART data (`uart::pollBridge()`), or the debug log in a `MUPP_DEBUG_CDC` build.

Each call sends one IN packet of up to 64 bytes: 64 bytes per ms.
At 115200 baud, the UART receives 11.5 bytes per ms.

The call makes one clock phase longer by the time of the service, some microseconds.
The clock does not get faster: after a late edge, the next half period is complete (see [Clock pacing](clock-pacing.md)).
For JTAG in timed mode, the low half period starts again after the call.
Thus TDI and TMS change a full half period before the next rising edge, as for all other bits.
For SWD, each bit waits a full half period after the SWDIO write, so SWDIO also keeps its setup time.
The duty cycle of that one clock period changes.
The service takes some microseconds in each millisecond, so a shift gets a little slower.
The time of the service on the target is not measured yet.

Limits:

- A SETUP packet from the host stops the serial port until the command ends.
  For example, a terminal program sends SET_LINE_CODING when it opens the port.
- The `DELAY` block does not call the service (see above).
- The service runs at a byte boundary. At a very slow clock, the time between two calls is 1 ms plus one byte.
  At 10 kHz TCK, this is 1.8 ms.
- With adaptive clocking (RTCK), one byte has no such limit.
  Each of the 16 TCK edges of a byte can wait up to 10 ms for RTCK, so one byte can take up to 160 ms.
  This is more than the 89 ms of the RX ring buffer.
  Data loss occurs only when the target replies to each edge after more than about 5.5 ms.

### JTAG_RAW block

OpenOCD shifts all JTAG bits through the `JTAG_RAW` block (`src/protocol/versaloon/JtagRawBlock.h`).
The firmware drives the pins with GPIO bit-bang (`src/jtag/JtagBitBang.h`).
On the boards with the [JTAG fast path](#jtag-fast-path), SPI and DMA shift the whole bytes.
The pins are TCK, TMS, TDI, TDO and RTCK in the table of the [GPIO block](#gpio-block).
TRST and SRST stay in the `GPIO` block.

The sub-commands use port index 0. The values are little-endian:

| Sub-command | Payload | Reply |
|---|---|---|
| `INIT` | none | `[status]` |
| `FINI` | none | `[status]` |
| `CONFIG` | `[kHz u32]` | `[status]` |
| `IN_OUT` | `[bitlen u32][TDI n bytes][TMS n bytes]` | `[status][TDO n bytes]` |

The byte count n is `ceil(bitlen / 8)`.
The first bit of a stream is the LSB of the first byte.
In the last TDO byte, the bits above `bitlen` are 0.

`INIT` makes TCK, TMS and TDI push-pull outputs, with TMS and TDI high.
TCK is low, or high on a board with the fast path.
TDO becomes an input with pull-up, so an open TDO reads 1.
RTCK becomes an input with pull-down.
`INIT` also sets TCK to 1000 kHz.
`FINI` makes TCK, TMS and TDI floating inputs.

`CONFIG` sets the TCK frequency in kHz.
The firmware rounds the half period up to full core clock cycles (72 MHz), so TCK is never faster than the request.
A request above the fastest rate of the bit-bang loop gives that fastest rate.
On a board with the fast path, `CONFIG` can select the SPI instead (see [JTAG fast path](#jtag-fast-path)).
An interrupt can make a TCK period longer, but not shorter.

`CONFIG` with 0 kHz selects adaptive clocking.
After each TCK edge, the firmware waits until RTCK has the same level.
When RTCK does not follow within 10 ms, `IN_OUT` stops and replies `TIMEOUT`.
TCK is then low, or high when RTCK did not follow a rising edge on a board with the fast path.
The original firmware did not report this error.
A board without RTCK (the ST-Link/V2-1) replies `INVALID_PARA` to 0 kHz.

For each bit, the firmware writes TDI and TMS while TCK is low, and waits for the low half period.
When TCK idles high, each bit starts with the falling edge.
Then it reads TDO and sets TCK high: the target samples TDI and TMS at this rising edge.
After the high half period, TCK goes low, and the target changes TDO.

A wrong payload size gets `INVALID_PARA`, and a port index other than 0 gets `INVALID_INDEX`.
`IN_OUT` replies the n TDO bytes also when it fails, as zeros.

### JTAG fast path

On the Mini, the WeAct STM32H523 and the YD-CH32V307VCT6, two SPI peripherals and DMA shift the whole bytes of an `IN_OUT` (`src/jtag/JtagFastPath.h`).
The STM32F1 driver is `src/board/BoardJtagFast.cpp` (SPI and DMA1). The STM32H5 driver is `src/board/BoardJtagFastH5.cpp` (SPI v2 and GPDMA1).
The CH32V307 driver is `src/board/BoardJtagFastV30x.cpp` (SPI2, SPI3, DMA1 and DMA2). It is tested from 1125 to 9000 kHz (see [CH32V307 JTAG](clock-pacing.md#ch32v307-jtag)).
The bit-bang engine shifts the remaining bits, and all bits at a bit-bang speed.
A board has the fast path when its definition has `jtagFastPath` (see [Boards](boards.md)).
The ST-Link/V2-1 does not have it.

| Signal | Pin | Peripheral | STM32F1 DMA1 channel | STM32H5 GPDMA1 channel | CH32V307 DMA channel |
|---|---|---|---|---|---|
| TCK | PB13 | SPI2 SCK, master | - | - | - |
| TDO | PB14 | SPI2 MISO | 4 | 1 | DMA1 4 |
| TDI | PB15 | SPI2 MOSI | 5 | 2 | DMA1 5 |
| TCK link | PB3 (CH32V307: PC10) | SPI1 SCK, slave (remapped on the STM32F1). CH32V307: SPI3 SCK, slave, remapped | - | - | - |
| TMS | PB4 (STM32F1), PA6 (STM32H5), PC11 (CH32V307) | SPI1 MISO (remapped on the STM32F1). CH32V307: SPI3 MISO, remapped | 3 | 0 | DMA2 2 |

The board links TCK (PB13) to the slave SCK pin, so TCK also clocks the slave SPI.
The slave SPI is SPI1, or SPI3 on the CH32V307.
Both SPIs use clock polarity high and the second clock edge, LSB first.
TDI and TMS change at the falling edge.
The target and SPI2 sample at the rising edge.
The SPI clock idles high, so TCK also idles high between shifts.
Only the rising edge clocks the TAP, so the idle level has no effect on the target.

The slave SPI makes TMS valid a time tv(SO) after the falling edge.
The half period must be longer than tv(SO), the delay of the TCK link and the TMS setup time of the target.
The smallest SPI divider is `jtag::spiMinShift`:

| Family | SPI2 clock | SPI speeds (kHz) | Divider off | tv(SO) |
|---|---|---|---|---|
| STM32F1 | 36 MHz APB1 clock, divided by 4 to 256 | 9000, 4500, 2250, 1125, 562.5, 281.25, 140.625 | 2 (18 MHz, half period 27.8 ns) | 25 ns (STM32F103x8/B datasheet) |
| STM32H5 | 125 MHz kernel clock (PLL1 Q), divided by 16 to 256 | 7812.5, 3906.25, 1953.1, 976.6, 488.3 | 4 (31.25 MHz, half period 16 ns) and 8 (15.6 MHz, not tested) | 11.5 ns on PA6 (DS14540 table 109) |
| CH32V307 | 144 MHz APB1 clock, divided by 16 to 256 | 9000, 4500, 2250, 1125, 562.5 | 4 (36 MHz) and 8 (18 MHz, half period 27.8 ns, not tested) | 10 ns (CH32V307DS0 table 4-28). Measured: 4 to 10 ns after the edge on PC10, with a wire as the TCK link |

Enable a smaller divider only after a hardware test at that speed passes.
`CONFIG` selects the engine with the faster TCK, and TCK is never faster than the request:

- The SPI gives the fastest SPI speed that is not above the request.
- Bit-bang gives the request. For the choice, its speed counts as the request, up to `jtag::bitBangMaxKHz`: 1000 kHz on the STM32F1 and the CH32V307, 3000 kHz on the STM32H5.
- The SPI wins when its speed is equal to or higher than the bit-bang speed.
- Adaptive clocking (0 kHz) always uses bit-bang.

STM32F1 examples: 9000 kHz and more give 9 MHz SPI, 4500 kHz gives 4.5 MHz SPI, 4000 kHz gives 2.25 MHz SPI, 2000 kHz gives 1.125 MHz SPI, 1000 kHz gives 1 MHz bit-bang.
The CH32V307 has the SPI speeds of the STM32F1, so the STM32F1 examples apply to it too.
On the CH32V307, the bit-bang engine gives 1106 kHz at a request of 1124 kHz (see [CH32V307 JTAG](clock-pacing.md#ch32v307-jtag)).
STM32H5 examples: 7813 kHz and more give 7.8 MHz SPI, 4000 kHz gives 3.9 MHz SPI, 3000 kHz gives bit-bang, 2000 kHz gives bit-bang.
On the STM32F1, the value of `jtag::bitBangMaxKHz` is conservative: 1000 kHz is the rate that was checked with the flash benchmark.
The fastest rate of the bit-bang loop on the STM32F1 is not measured.
A higher value selects bit-bang for some requests above 1125 kHz, and the SPI is then slower than the request less often.
To raise the value, set it to 18000 for a test, and measure TCK at a request of 4000 kHz with a logic analyzer.
On the STM32H5, the bit-bang engine gives 2.87 MHz at a request of 3000 kHz, and 3.56 MHz at 3900 kHz (see [Clock pacing](clock-pacing.md#stm32h5-jtag-fast-path)).
Both rates are higher than the SPI speed below them (1953 kHz).

A long shift runs in SPI chunks of about 1 ms, and the [serial service](#serial-port-during-long-commands) runs between two chunks.
The pins are GPIO between two transfers, so the bit-bang engine and the SWD engine can use them.
SPI1 also counts the TCK edges of bit-bang and SWD, so each transfer resets SPI1.

Limits:

- The STM32F1 SPI1 slave has no underrun flag. When DMA is late with a TMS byte, a TMS bit is wrong and the firmware does not see it.
  DMA channel 3 has the highest priority to prevent this.
  On the STM32H5, the SPI1 slave sets its underrun flag (UDR), and the shift fails.
  The flag comes a few TCK cycles after the underrun, so an underrun in the last frames of a transfer is not seen.
  The STM32H5 shift also fails when SPI2 loses a TDO byte (OVR).
- At 9 MHz, the half period is 55.6 ns (at 7.8 MHz on the STM32H5, 64 ns). The target clock-to-output time and the cable delay must be less than this, else the TDO bits are wrong.
  Use a lower speed with a long cable.
- The OpenOCD flash speed with the fast path is measured with `tools/flash_benchmark.py` (see [docs/development.md](development.md#flash-speed-benchmark)).
  On the STM32F1, at 4500 and 9000 kHz, the total flash time is 1 to 2 % longer than with the original firmware.
  The flash speed on the STM32H5 is not measured.

### SWD block

OpenOCD runs all SWD transfers through the `SWD` block (`src/protocol/versaloon/SwdBlock.h`).
The firmware drives the pins with GPIO bit-bang (`src/swd/SwdBitBang.h`).
SWCLK is the TCK pin.
SWDIO is the TMS pin: the firmware drives the TMS output pin and reads the TMS input pin (PB4 and PB6 on the Mini).

The sub-commands use port index 0. The values are little-endian:

| Sub-command | Payload | Reply |
|---|---|---|
| `INIT` | none | `[status]` |
| `FINI` | none | `[status]` |
| `CONFIG` | `[trn u8][retry u16][delay u16]` | `[status]` |
| `OUT` (sequence out) | `[bitlen u16][data n bytes]` | `[status]` |
| `IN` (sequence in) | `[bitlen u16]` | `[status][data n bytes]` |
| `IN_OUT` (transfer) | `[request u8][data u32]` | `[status][ack u8][data u32]` |

The byte count n is `ceil(bitlen / 8)`.
The first bit of a sequence is the LSB of the first byte.

`INIT` makes SWCLK and SWDIO push-pull outputs at the high level.
A separate SWDIO input pin gets a pull-up, so an open SWDIO reads 1.
`INIT` also sets the default configuration: one turnaround cycle, no retry, and delay 1 (235 kHz).
`FINI` makes SWCLK and the SWDIO output pin floating inputs.

`CONFIG` has these values:

- `trn`: the turnaround cycles of one transfer, for both turnarounds together, as in the original firmware.
  OpenOCD sends 2, which is one cycle at each turnaround.
  The firmware accepts 2, 4, 6 and 8, and replies `INVALID_PARA` to other values.
- `retry`: the number of times that the firmware repeats a transfer after a WAIT acknowledge.
- `delay`: the SWCLK frequency, as the OpenOCD `vsllink` driver calculates it:

| `delay` | 0 | 1 | 2 | 3 | 4 | 5 | 6 and more |
|---|---|---|---|---|---|---|---|
| SWCLK | 1.85 MHz | 235 kHz | 130 kHz | 102 kHz | 85 kHz | 72 kHz | 500000 / (`delay` + 1) Hz |

The firmware rounds the half period up to full core clock cycles (72 MHz).
Each SWCLK edge comes at a deadline of the core cycle counter.
Thus the loop code does not change the SWCLK rate (see [Clock pacing](clock-pacing.md)).
An interrupt makes a period longer, but not shorter.

At `delay` 0, the firmware runs untimed, without the cycle counter.
Each half period is the loop code and 6 NOP instructions (`swd::untimedPadNops`).
The NOPs keep a minimum low half period: SWDIO has a setup time before the rising edge.
Thus the SWCLK rate depends on the code. A logic analyzer measured 2.42 to 2.7 MHz on the Mini, faster than the 1.85 MHz of the table (see [Clock pacing](clock-pacing.md)).
The original firmware bursts at 2.25 to 3 MHz.
If a target or a long cable fails at `delay` 0, use `delay` 1 (OpenOCD `adapter speed` below 1850 kHz).
On the STM32H5 (250 MHz), `delay` 0 is timed at 1.85 MHz: the untimed engine there gives 13 MHz (`swd::runsUntimed()`, see [Clock pacing](clock-pacing.md#stm32h5)).

`OUT` drives the bits on SWDIO. `IN` releases SWDIO and reads the bits.
SWCLK is high between bits.
In each bit, SWCLK goes low, the firmware writes SWDIO, and waits for the low half period.
Then the firmware reads SWDIO, and sets SWCLK high: the target samples SWDIO at this rising edge.

`IN_OUT` runs one transfer:

1. The firmware sets the start bit, the parity, the stop bit and the park bit of the request.
   OpenOCD sends them already. Bits 1-4 of the request (APnDP, RnW, A2, A3) stay.
2. The firmware sends the request, releases SWDIO for the turnaround, and reads the 3-bit acknowledge.
3. The firmware runs the data phase for each acknowledge, as the OpenOCD `bitbang` driver and the original firmware do.
   For a read, it reads 32 data bits and the parity bit with SWDIO released, then clocks the turnaround.
   Only an OK acknowledge gives data.
   For a write, it clocks the turnaround, then sends the 32 data bits and the parity bit.
   OpenOCD sets ORUNDETECT in the DP. Then the target expects the data phase also after WAIT and FAULT.
   A write without an acknowledge (multidrop `TARGETSEL`) also needs the data phase.
4. The firmware drives SWDIO low for 8 idle cycles, as the original firmware does.
   The target needs idle cycles to complete a write, and OpenOCD does not send them to this adapter.
5. After WAIT, the firmware writes DP `ABORT` with `ORUNERRCLR` (`0x10`), and then repeats the transfer.
   It does this up to `retry` times.
   With ORUNDETECT, a WAIT sets `STICKYORUN`, and the DP replies FAULT until `ORUNERRCLR` clears it.

The `ack` byte has the acknowledge in bits 0-2 (OK 1, WAIT 2, FAULT 4), and these flags:

| Flag | Meaning |
|---|---|
| `0x80` | FAULT acknowledge |
| `0x40` | WAIT acknowledge after all retries |
| `0x20` | Acknowledge that is not OK, WAIT or FAULT. No target gives 7. |
| `0x10` | Parity error in the read data |

The reply data is the read data.
For a write, and for a read that is not OK, the reply data is the request data.
The status is `OK` only when the `ack` byte is 1 (OK without a parity error). Else the status is `FAILED`.
A DP `TARGETSEL` write (multidrop SWD, DP address `0xC`) is the exception: its status is always `OK`.
No target drives the acknowledge of `TARGETSEL`, so its `ack` byte is usually `0x27`.
The OpenOCD `bitbang` driver also ignores this acknowledge (`swd_cmd_returns_ack()`).
The `ack` byte keeps the value from SWDIO.
OpenOCD does not read the `ack` byte, so it sees a failed transfer only through the status.
The original firmware replied `OK` in all cases.

A wrong payload size gets `INVALID_PARA`, and a port index other than 0 gets `INVALID_INDEX`.
`IN` and `IN_OUT` reply their full data size also when they fail.

### POWER block

The `POWER` block (`src/protocol/versaloon/PowerBlock.h`) switches the 3.3 V supply of the target and measures the target voltage (Vref).
OpenOCD does not use it, but other tools do.
The sub-commands use port index 0:

| Sub-command | Payload | Reply |
|---|---|---|
| `INIT` | none | `[status]` |
| `FINI` | none | `[status]` |
| `CONFIG` | none | `[status]` |
| `OUT` | `[mV u16]` | `[status]` |
| `IN` | any, the firmware ignores it | `[status][mV u16]` |

`OUT` accepts only 0 (supply off) and 3300 (supply on).
Other values give `INVALID_PARA`.
`IN` replies the voltage at the Vref pin.
The voltage is the supply of the adapter when it is on, and the supply of the target when it is off.
`GET_TVCC` replies the same voltage.

The Mini has the power switch on PB8.
The switch is on when PB8 is driven low, and off when PB8 floats.
The Vref sense is on PB0 (ADC1 channel 8) with a divider of 2.
The firmware calculates the voltage from the mean of 8 conversions, with a 3.3 V ADC reference.

The firmware protects the target and the adapter.
`ID_DAP_Vendor2` of the CMSIS-DAP personality uses the same protections (`src/protocol/TargetPower.h`):

- `OUT 3300` gives `FAILED` and keeps the supply off when the voltage is above 1.8 V.
  The target then has its own supply, and two supplies on one rail can do damage.
- `OUT 3300` gives `FAILED` and keeps the supply off when the firmware cannot measure the voltage.
- The firmware switches the supply on, and waits 10 ms.
  When the voltage is then not above 1.8 V, the rail has a short circuit or an overload.
  The firmware then switches the supply off again and replies `FAILED`.
- While the supply is on, the main loop measures the voltage every 10 ms.
  The firmware switches the supply off when 3 measurements in a row are not above 1.8 V.
  A failed measurement does not count.

The supply stays on when the host goes away, so that a tool can switch the target on and then end.
It does not stay on after a reset of the adapter: the switch pin floats then.

A board can have no switch or no sense (see [Boards](boards.md)):

| Board has | `OUT 0` | `OUT 3300` | `IN` and `GET_TVCC` |
|---|---|---|---|
| Switch and sense | `OK` | With the protection above | Voltage |
| Sense, no switch | `OK` | `CMD_NOT_SUPPORT` | Voltage |
| Switch, no sense | `OK` | `OK`, with no protection, and no automatic switch-off | `IN`: `CMD_NOT_SUPPORT`, `GET_TVCC`: 0 |
| Neither | `OK` | `CMD_NOT_SUPPORT` | `IN`: `CMD_NOT_SUPPORT`, `GET_TVCC`: 0 |

The ST-Link/V2-1 has a sense but no switch.

## Check the enumeration

The host unit tests check the descriptors.
To check the device on the hardware, connect the dongle and do these steps.

On Linux:

1. Run `lsusb -v -d 0483:a038`. Make sure that the output shows the interfaces and endpoints of the tables above, and the IAD.
2. Make sure that the kernel log (`dmesg`) shows a new `/dev/ttyACM*` port.
3. Connect the UART TX pin to the UART RX pin (PA9 to PA10 on the Mini). Open the port in a terminal program, for example `picocom /dev/ttyACM0`. Make sure that the typed characters come back.
4. Run OpenOCD with `-f interface/vsllink.cfg`.
   Make sure that OpenOCD shows the version text of `GET_INFO`, for example `openmupp v0.1.0, board mini`.
   Make sure that OpenOCD shows the capabilities, `USB_TO_XXX abilities: 0x00000008:0x000000C0:0xC0000002`.
   In JTAG mode, OpenOCD then scans the JTAG chain.
   In SWD mode, OpenOCD reads the DPIDR of the target.

On Windows 10 or later:

1. Make sure that the Device Manager shows a new COM port under "Ports (COM & LPT)", with the built-in CDC driver (`usbser.sys`).
2. Make sure that interface 0 shows as a separate device ("Versaloon").
   OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.


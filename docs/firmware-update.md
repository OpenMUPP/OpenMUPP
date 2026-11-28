# Firmware update

OpenMUPP v1 (CH32V307) runs the `app-8k` image behind the DFU bootloader of `openmupp-bootloader`.
See [OpenMUPP v1: DFU bootloader](#openmupp-v1-dfu-bootloader).

The Mini runs the firmware behind a bootloader: `openmupp-bootloader` (8 KB) or a Versaloon bootloader (8 KB, or 32 KB on the newer Mini).
The ST-Link/V2-1 and the ST-Link/V2 mini clone run it behind `openmupp-bootloader`.
See [Install the bootloader](#install-the-bootloader).
On these STM32F1 boards, `openmupp-bootloader` is a USB CDC serial port with the ID `0483:5740`.
It uses the STM32 serial bootloader protocol (ST AN3155), so `stm32flash` can write the firmware.

## Start the bootloader

To start `openmupp-bootloader`, use one of these methods:

- Send the Versaloon command `VERSALOON_FW_UPDATE` to interface 0: the two bytes `0x0F 0xAA`. Only the Versaloon personality has this command.
- Send a switch command with the bootloader code `0xB0` (see [Bootloader code](#bootloader-code)). Each personality has a switch command.
- On the Mini: hold the key (PB9) down and reset the board.
- On the ST-Link/V2 mini clone: connect SWIM (header pin 3) to GND (header pin 5), then connect the board to USB (see [Bootloader](stlinkv2-mini-clone.md#bootloader)).
- On the ST-Link/V2-1: connect SWO (SWD connector pin 6) to GND (pin 3), then connect the board to USB. Only the bootloader reads this key, so the firmware can use PA10 for SWO.
  The bootloader reads this key at every reset, and the personality switch also goes through a reset.
  Thus an unpowered target, or a Nucleo target that holds PB3 low, can keep the probe in the bootloader.
  On an MB1136 Nucleo that is not broken off, SWO also connects to PB3 of the target MCU through SB15. If the target drives PB3 low, open SB15. Other Nucleo boards use other bridges. Read the user manual of your board.

Only the `app-8k` image accepts the commands.
On OpenMUPP v1, the key does not start `openmupp-bootloader`: it starts the ROM ISP bootloader of WCH (see [Start the DFU bootloader](#start-the-dfu-bootloader)).
Behind the 8 KB Versaloon bootloader, the firmware only starts again, because that bootloader does not know the boot request.
To start a Versaloon bootloader, hold the key (PB9) down while you connect the board to USB.

### Bootloader code

The switch commands of the personalities also start the bootloader.
Send the bootloader code `0xB0` (`boot::bootloaderCode`) instead of a personality code:

| Personality | Command | Reply when the firmware accepts |
|---|---|---|
| Each | Switch request on EP0: `bmRequestType` `0x40`, `bRequest` `0x0E`, `wValue` `0xB0` (see [Switch request on EP0](usb-device.md#switch-request-on-ep0)) | Status stage (ACK) |
| Versaloon | `SWITCH_PERSONALITY`: `[0x0E][0xB0]` | `[0x00]` |
| CMSIS-DAP | `ID_DAP_Vendor0`: `[0x80][0xB0]` | `[0x80][0x00]` |

The WCH-Link protocol has no switch command, so use the switch request on EP0.
The commands also work in an image with one personality.
An image other than `app-8k` refuses the code.
The switch request on EP0 then stalls, and `SWITCH_PERSONALITY` replies status `1`.
A multi image replies status `0xFF` to `ID_DAP_Vendor0`.
A CMSIS-DAP image with one personality replies `ID_DAP_Invalid` to `ID_DAP_Vendor0`.
The exception is the `standalone` image of the WeAct STM32H523, an earlier development board: it accepts the code and starts the ROM USB DFU of the STM32H5.
After the reply, the firmware does the same steps as for `VERSALOON_FW_UPDATE` (see [Reply and steps](#reply-and-steps)).

`tools/mupp_probe.py` sends the bootloader code from the host:

```sh
python3 tools/mupp_probe.py switch bootloader
```

The tool sends the switch request on EP0 with the code `0xB0`.
A firmware from before the bootloader code stalls the request.
Then the tool sends `VERSALOON_FW_UPDATE` in the Versaloon personality, and `ID_DAP_Vendor0` with `0xB0` in the CMSIS-DAP personality.
After the request, the tool waits 10 s for the bootloader.
It finds `openmupp-bootloader` by the ID `0483:5740`, the product string `Bootloader` and the serial number of the dongle.
Other devices use the ID `0483:5740` too, so the tool also compares the product string.
It finds the DFU bootloader of OpenMUPP v1 by the ID `1209:0001`, the product string `OpenMUPP Bootloader` and the serial number.
`python3 tools/mupp_probe.py show` also lists a dongle that runs the bootloader.

The tool must open the bootloader to read its strings.
On Linux, a udev rule gives a normal user this access.
Copy `etc/70-openmupp-bootloader.rules` to `/etc/udev/rules.d/`.
Then reload the rules with `udevadm control --reload`, and start the bootloader again.
Without access, the tool does not find the bootloader.
It then stops after 10 s with an error and a hint, also when the bootloader runs.

The tool tells about each device with a known ID whose strings it cannot read.
The message gives the ID, the bus and the address of the device.
If you see this message, install the udev rules in `etc/`.
The option `--id` does not fix this: the tool must open the device to send the request.

### Stored personality

A multi image stores its personality in the flash page at `0x0800FC00`.
In the `app-32k` image, the page is at `0x0801FC00`.
An image with the RISC-V bridge stores the bridge mode in the same page (see [Stored mode](cmsis-dap.md#stored-mode)).
`stm32flash -w` erases only the pages of the image, so an update keeps the stored personality and the stored mode.
A mass erase of the flash also erases this page.
The firmware then starts with its default personality.

### Reply and steps

The firmware replies to `VERSALOON_FW_UPDATE` with one status byte:

| Status | Meaning |
|---|---|
| `0x00` | Accepted. The firmware starts the bootloader. Behind the 8 KB Versaloon bootloader, the firmware only starts again, because that bootloader does not know the boot request. |
| `0x01` | Refused. The second byte is not the key `0xAA`, or the image is not `app-8k`. On the STM32F1, the `standalone` image has no bootloader. The 32 KB Versaloon bootloader of the `app-32k` image does not know the boot request. The firmware continues to run. |

On the STM32F1, after the reply, the firmware does these steps:

1. It waits 10 ms, so the host can read the reply. The firmware does not wait for the host to read the reply.
2. It enables the `PWR` and `BKP` clocks and the write access to the backup domain.
3. It writes the boot request `0xB007` to the backup register `BKP->DR1`.
4. It resets the MCU with `NVIC_SystemReset()`. The host sees a USB disconnect.

After the reset, the bootloader finds the boot request, clears it and stays in bootloader mode.
The next reset starts the firmware again.
`src/boot/BootloaderEntry.h` and `src/boot/Bootloader.cpp` have the code.

## OpenMUPP v1: DFU bootloader

On OpenMUPP v1, `openmupp-bootloader` is a USB DFU 1.1 device with the ID `1209:0001` and the product string `OpenMUPP Bootloader`.
The firmware uses the same pid.codes test ID, so the tools also compare the product string.
The serial number is the chip UID, as in the firmware.
The bootloader is not an AN3155 device, so use `dfu-util`, not `stm32flash`.

### Start the DFU bootloader

- Send a switch command with the bootloader code `0xB0`: `python3 tools/mupp_probe.py switch bootloader`.
  The tool waits for `1209:0001` with the product string `OpenMUPP Bootloader`.
- The firmware writes the boot request `0xB007` to `BKP_DATAR1` and resets the MCU (`src/boot/BootloaderV30x.cpp`).

The key of OpenMUPP v1 is on BOOT0.
A key that is down at a reset starts the ROM ISP bootloader of WCH (`4348:55e0`, `wchisp`), not `openmupp-bootloader`.
Use the ROM bootloader when `openmupp-bootloader` is damaged.

### Write the firmware with dfu-util

```sh
dfu-util -d 1209:0001 -D openmupp-app-8k.bin
```

- The bootloader writes the image from `0x08002000`. The host cannot select an address.
- After the download, the bootloader checks the image header and starts the firmware.
  With an image that is not valid, it reports `errVERIFY` and stays in DFU mode.
- The bootloader erases only the 4 KB sectors of the image.
  The write area ends at the end of the 256 KB zero-wait flash, so the `app-8k` image can be up to 248 KB (253952 bytes).
  The settings page of the firmware is behind this area, so a download keeps the settings.
- On Linux, `dfu-util` and `mupp_probe.py` need access to the device.
  `etc/70-openmupp-bootloader.rules` has a udev rule for `1209:0001` with the product string `OpenMUPP Bootloader`.

The `app-8k` image of OpenMUPP v1 links at `0x00002000`, the flash alias of `0x08002000`, because the bootloader jumps to the alias.
The build also makes `openmupp-with-bootloader.bin`: the bootloader and the `app-8k` image in one file from `0x08000000`, for a new board and a debug probe.

## Install the bootloader

The ST-Link/V2-1 and the ST-Link/V2 mini clone use `openmupp-bootloader`.
Their original firmware has read protection.
Remove it before you write the combined image:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "init; reset halt; stm32f1x unlock 0; exit"
```

The unlock erases all of the flash.
Then disconnect the power of the board, and connect it again.
The read protection stays active until the next power-up.
See [Read protection of the original firmware](stlinkv2-mini-clone.md#read-protection-of-the-original-firmware).
On the ST-Link/V2-1, connect the debug probe to the SWD pads of the MCU (see [ST-Link/V2-1](boards.md#st-linkv2-1)).

The Mini uses one of these bootloaders:

- The Versaloon bootloader uses 8 KB or 32 KB. The existing boards have it.
  The 8 KB bootloader runs the `app-8k` image.
  The 32 KB bootloader is on the newer Mini, with 128 KB flash. It runs the `app-32k` image.
  Hold the key (PB9) down while you connect the board to USB to start it.
  It does not know the boot request, so `VERSALOON_FW_UPDATE` does not start it.
- `openmupp-bootloader` uses 8 KB. It runs the `app-8k` image.
  The submodule `dependencies/openmupp-bootloader` has its source (https://github.com/OpenMUPP/openmupp-bootloader).

A board with a Versaloon bootloader can continue to use it.
To change a board to `openmupp-bootloader`, write the new bootloader one time with a debug probe.
Then write the `app-8k` image, or write the combined image to do both in one step.

The firmware build makes the combined image `openmupp-with-bootloader.hex` (see [Other boards](../README.md#other-boards)).
It has the bootloader at `0x08000000` and the `app-8k` image at `0x08002000`.
Write it with a debug probe on the SWD pins of the dongle MCU, for example with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program openmupp-with-bootloader.hex verify reset exit"
```

The build also makes the bootloader image alone, `openmupp-bootloader/openmupp-bootloader-<board>.hex`.
Write it in the same way when the board must keep its firmware.

At its first start, `openmupp-bootloader` write-protects its 8 KB area.
To write a new bootloader over it, remove the write protection first:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "init; reset halt; stm32f1x unlock 0; reset halt; exit"
```

`stm32f1x unlock 0` erases the option bytes and removes all write protection.
The section [Flash protection](https://github.com/OpenMUPP/openmupp-bootloader/blob/master/docs/stm32f1.md#flash-protection) of the bootloader document "STM32F1 boards" has the full rules.

The Versaloon bootloader does not set write protection or read protection.
Thus the debug probe can write the combined image over it without `stm32f1x unlock 0`.
If the write fails, the option bytes of the board were changed: run `stm32f1x unlock 0` first.

## Write the firmware

1. Start the bootloader (see above). On Linux, a new port `/dev/ttyACM*` shows.
   `openmupp-bootloader` has the ID `0483:5740`.
2. Write the `.bin` file of the image for the bootloader:

   ```sh
   # openmupp-bootloader or Versaloon bootloader (8 KB)
   stm32flash -w openmupp-app-8k.bin -v -S 0x08002000 /dev/ttyACM0
   # Versaloon bootloader (32 KB)
   stm32flash -w openmupp-app-32k.bin -v -S 0x08008000 /dev/ttyACM0
   ```

3. Reset the board, or disconnect it and connect it again. The firmware starts.
   With `openmupp-bootloader`, `stm32flash -g 0x08002000` also starts it.

Do not write the `standalone` image through a bootloader.
It is linked at `0x08000000`, so it does not start behind a bootloader.
Do not write the `app-32k` image behind an 8 KB bootloader, or the `app-8k` image behind the 32 KB bootloader.
The start address of the image must be the end of the bootloader.

## Application image header

`openmupp-bootloader` starts only an application with a valid header.
Thus it does not start a half-written or damaged image: it stays in bootloader mode.
The header is in the reserved words of the vector table:

| Offset | Content |
|---|---|
| `0x1C` | Magic `0x314C5356` (`"VSL1"` in memory) |
| `0x20` | Image length in bytes |
| `0x24` | CRC32 of the image (algorithm of the STM32F1 CRC unit) |
| `0x28` | Reserved, `0` |

The CRC covers the linked image, so a step after the link must add the header.
The build calls `mupp_stamp_image()` from the bootloader submodule (`cmake/MuppStampImage.cmake`) for the `app-8k` and `app-32k` images.
It writes the header into the `.elf` file, and it writes the `.hex` and `.bin` files.
The Versaloon bootloaders ignore the header.
The `standalone` image has no header.

To check an image, use the stamp tool with the start address:

```sh
dependencies/openmupp-bootloader/tools/mupp-stamp-image.py --check openmupp-app-8k.bin --base 0x08002000
```

## Start-up behind the bootloader

The bootloader starts the firmware after a system reset.
Thus all peripherals are in their reset state when the firmware starts.
The bootloader sets `VTOR` to the firmware vector table at the start of the application.
The system-stm32 start-up code sets `VTOR` again from the linker script.
The bootloader jumps to the firmware with all interrupts masked (`PRIMASK` set).
`os_startup_initialize_hardware()` clears `PRIMASK` after the pin setup, before the static constructors run.
Without this step, the SysTick and USB interrupts never run, and the host cannot enumerate the device.
The firmware initializes its DATA and BSS sections. It does not use RAM content from the bootloader.

## Flash layout and size limit

The Mini can have an STM32F103C8 with 64 KB flash.
The `standalone` and `app-8k` images use only 64 KB flash.
The `app-32k` image uses 128 KB flash: the 32 KB bootloader is only on parts with 128 KB flash or more.
The bootloader uses the start of the flash, and the application uses the rest:

| Image | Bootloader | Application | Size limit | Size limit, multi image or RISC-V bridge |
|---|---|---|---|---|
| `standalone` | none | `0x08000000` | 64 KB | 63 KB |
| `app-8k` | `openmupp-bootloader` or Versaloon bootloader, 8 KB | `0x08002000` | 56 KB | 55 KB |
| `app-32k` | Versaloon bootloader, 32 KB | `0x08008000` | 96 KB | 95 KB |

The limits are hard limits for all STM32F1 boards.
OpenMUPP v1 has other limits: `standalone` 256 KB, `app-8k` at `0x08002000` with 248 KB (see [OpenMUPP v1: DFU bootloader](#openmupp-v1-dfu-bootloader)).
The `app-8k` limit is the smallest. CI checks each image against its limit (see [Continuous integration](development.md#continuous-integration)).

- The FLASH region of the link (`__flash_length`) goes from the start of the application to the settings page, or to the end of the flash (64 KB, or 128 KB for `app-32k`). With `MUPP_STRICT_SIZE=ON`, the link of an image fails when the application is too large. The CI build jobs set this option.
- Without `MUPP_STRICT_SIZE`, the build skips an image that is too large, and shows a warning (see [Images that do not fit](cmake.md#images-that-do-not-fit)).
- CI reports the size of each image with `ci/check-size.sh` and fails when the `.bin` file is larger than its limit.

The main stack is 2 KB (`__main_stack_size` of the system-stm32 `sections.ld`), because the STM32F103 has only 20 KB RAM.
The link fails when less than the full main stack is free.


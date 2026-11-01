# Firmware update

The Mini runs the firmware behind a bootloader: `openmupp-bootloader` (8 KB) or the legacy Versaloon bootloader (10 KB).
The ST-Link/V2 and the ST-Link/V2 mini clone run it behind `openmupp-bootloader`.
See [Install the bootloader](#install-the-bootloader).
`openmupp-bootloader` is a USB CDC serial port with the ID `0483:5740`.
It uses the STM32 serial bootloader protocol (ST AN3155), so `stm32flash` can write the firmware.

The WeAct STM32H523 core board (STM32H5) has no bootloader in flash.
The firmware starts the USB DFU (Device Firmware Upgrade) of the ROM bootloader in system memory.
See [STM32H5: ROM USB DFU](#stm32h5-rom-usb-dfu).

## Start the bootloader

To start `openmupp-bootloader`, use one of these methods:

- Send the Versaloon command `VERSALOON_FW_UPDATE` to interface 0: the two bytes `0x0F 0xAA`. Only the Versaloon personality has this command.
- Send a switch command with the bootloader code `0xB0` (see [Bootloader code](#bootloader-code)). Each personality has a switch command.
- On the Mini: hold the key (PB9) down and reset the board.
- On the ST-Link/V2 mini clone: connect SWIM (header pin 3) to GND (header pin 5), then connect the board to USB (see [Bootloader](stlinkv2-mini-clone.md#bootloader)).
  The ST-Link/V2 has no key.

Only the `app-8k` image accepts the commands.
On the STM32H5, the commands and the key start the ROM USB DFU (see [STM32H5: ROM USB DFU](#stm32h5-rom-usb-dfu)).
To start the legacy bootloader, hold the key (PB9) down while you connect the board to USB.

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
An image other than `app-8k` refuses the code, except on the STM32H5.
The switch request on EP0 then stalls, and `SWITCH_PERSONALITY` replies status `1`.
A multi image replies status `0xFF` to `ID_DAP_Vendor0`.
A CMSIS-DAP image with one personality replies `ID_DAP_Invalid` to `ID_DAP_Vendor0`.
After the reply, the firmware does the same steps as for `VERSALOON_FW_UPDATE` (see [Reply and steps](#reply-and-steps)).

`tools/vsl_personality.py` sends the bootloader code from the host:

```sh
python3 tools/vsl_personality.py switch bootloader
```

The tool sends the switch request on EP0 with the code `0xB0`.
A firmware from before the bootloader code stalls the request.
Then the tool sends `VERSALOON_FW_UPDATE` in the Versaloon personality, and `ID_DAP_Vendor0` with `0xB0` in the CMSIS-DAP personality.
After the request, the tool waits 10 s for the bootloader.
It finds `openmupp-bootloader` by the ID `0483:5740`, the product string `Bootloader` and the serial number of the dongle.
Other devices use the ID `0483:5740` too, so the tool also compares the product string.
It finds the ROM USB DFU of the STM32H5 by the ID `0483:df11` only.
The ROM DFU has its own serial number, so the wait for the bootloader does not compare it.
Thus any STM32 device in ROM DFU mode ends the wait, also a different board.
The other commands compare the serial number of the ROM DFU, so `--serial` does not select a device in ROM DFU mode.
`python3 tools/vsl_personality.py show` also lists a dongle that runs the bootloader.

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
An image with the RISC-V bridge stores the bridge mode in the same page (see [Stored mode](cmsis-dap.md#stored-mode)).
`stm32flash -w` erases only the pages of the image, so an update keeps the stored personality and the stored mode.
A mass erase of the flash also erases this page.
The firmware then starts with its default personality.

### Reply and steps

The firmware replies to `VERSALOON_FW_UPDATE` with one status byte:

| Status | Meaning |
|---|---|
| `0x00` | Accepted. The firmware starts the bootloader. |
| `0x01` | Refused. The second byte is not the key `0xAA`, or the image is not `app-8k`. On the STM32F1, the `standalone` image has no bootloader. The legacy bootloader of the `app-10k` image does not know the boot request. The firmware continues to run. |

The `standalone` image of the WeAct STM32H523 core board accepts the command and starts the ROM USB DFU (see [STM32H5: ROM USB DFU](#stm32h5-rom-usb-dfu)).

On the STM32F1, after the reply, the firmware does these steps:

1. It waits 10 ms, so the host can read the reply. The firmware does not wait for the host to read the reply.
2. It enables the `PWR` and `BKP` clocks and the write access to the backup domain.
3. It writes the boot request `0xB007` to the backup register `BKP->DR1`.
4. It resets the MCU with `NVIC_SystemReset()`. The host sees a USB disconnect.

After the reset, the bootloader finds the boot request, clears it and stays in bootloader mode.
The next reset starts the firmware again.
`src/boot/BootloaderEntry.h` and `src/boot/Bootloader.cpp` have the code.

## STM32H5: ROM USB DFU

The STM32H5 has a bootloader in system memory (ST AN2606, section 56).
On the STM32H523 and the STM32H533, it starts at `0x0BF97000`.
It has a USB DFU interface on PA11 and PA12 (the USB connector of the WeAct board), with the ID `0483:df11`.
The DFU uses the HSI48 clock, so it needs no crystal.

### Start the ROM DFU

The `standalone` image of the STM32H5 has the bootloader entry.
To start the ROM DFU, use one of these methods:

- Send `VERSALOON_FW_UPDATE` or a switch command with the bootloader code `0xB0` (see [Bootloader code](#bootloader-code)).
  `python3 tools/vsl_personality.py switch bootloader` does this and waits for `0483:df11`.
- Hold the key (KEY, PA0 on the WeAct board) down and reset the board.

The firmware does not jump to the ROM bootloader from the running firmware.
After the reply to the command, the firmware does these steps:

1. It waits 10 ms, so the host can read the reply.
2. It enables the RTC APB clock and the write access to the backup domain (`PWR_DBPCR.DBP`).
3. It writes the boot request `0xB007` to the backup register `TAMP->BKP0R`.
4. It resets the MCU with `NVIC_SystemReset()`. The host sees a USB disconnect.

A system reset keeps the backup registers.
After the reset, the start-up code reads `TAMP->BKP0R` before the clock set-up, and clears it.
When it finds the boot request, it jumps to the ROM bootloader.
At this point, the clocks, the PLL, the ICACHE and the interrupts are still in their reset state, as AN2606 section 4.1 requires.
Before the jump, the firmware sets `VTOR` to the ROM bootloader, clears `MSPLIM`, loads `MSP` from `0x0BF97000` and jumps to the reset vector at `0x0BF97004`.
The next reset starts the firmware again.

A key that is down at the reset gives the same boot request and reset.
`src/boot/RomBootloader.cpp` and `src/initialize_hardware.cpp` have the code.

The jump skips the secure firmware (SFSP) of the boot model Boot_V3_2 (AN2606 section 4.10).
This is correct only with TrustZone disabled (`TZEN` off), the default of a new STM32H5.
If the jump fails on a board, set the boot address in the option bytes instead.

### Write the firmware with dfu-util

1. Start the ROM DFU (see above). `dfu-util -l` shows the device `0483:df11` with the alternate setting 0 `@Internal Flash`.
2. Write the `.bin` file of the `standalone` image at `0x08000000`:

   ```sh
   dfu-util -a 0 -s 0x08000000:leave -D openmupp-standalone.bin
   ```

3. With `:leave`, the bootloader starts the firmware after the write.
   Without it, reset the board.

STM32CubeProgrammer can also write the firmware through the ROM DFU:

```sh
STM32_Programmer_CLI -c port=usb1 -w openmupp-standalone.bin 0x08000000 -v -s 0x08000000
```

On Linux, `dfu-util` needs access to the device.
`etc/70-openmupp-bootloader.rules` has a udev rule for `0483:df11` (see [Bootloader code](#bootloader-code)).

The write erases only the sectors of the image.
Thus a multi image or an image with the RISC-V bridge keeps the stored personality and the bridge mode in the last sector of bank 2, at `0x0803E000` on the STM32H523CC.

## Install the bootloader

The ST-Link/V2 and the ST-Link/V2 mini clone use `openmupp-bootloader`.
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

The Mini uses one of two bootloaders:

- The legacy Versaloon bootloader uses 10 KB. The existing boards have it. It runs the `app-10k` image.
  Hold the key (PB9) down while you connect the board to USB to start it.
  It does not know the boot request, so `VERSALOON_FW_UPDATE` does not start it.
- `openmupp-bootloader` uses 8 KB. It runs the `app-8k` image.
  The submodule `dependencies/openmupp-bootloader` has its source (https://gitlab.vandertogt.org/embedded-projects/openmupp-bootloader).

A board with the legacy bootloader can continue to use it.
To change a board to `openmupp-bootloader`, write the new bootloader one time with a debug probe.
Then write the `app-8k` image, or write the combined image to do both in one step.

The firmware build makes the combined image `openmupp-with-bootloader.hex` (see [Build the firmware](../README.md#build-the-firmware)).
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
The bootloader README has the full rules ("Flash protection").

The legacy bootloader (tag `v1-legacy` of `openmupp-bootloader`) does not set write protection or read protection.
Thus the debug probe can write the combined image over it without `stm32f1x unlock 0`.
If the write fails, the option bytes of the board were changed: run `stm32f1x unlock 0` first.

## Write the firmware

1. Start the bootloader (see above). On Linux, a new port `/dev/ttyACM*` shows.
   `openmupp-bootloader` has the ID `0483:5740`.
2. Write the `.bin` file of the image for the bootloader:

   ```sh
   # openmupp-bootloader (8 KB)
   stm32flash -w openmupp-app-8k.bin -v -S 0x08002000 /dev/ttyACM0
   # legacy bootloader (10 KB)
   stm32flash -w openmupp-app-10k.bin -v -S 0x08002800 /dev/ttyACM0
   ```

3. Reset the board, or disconnect it and connect it again. The firmware starts.
   With `openmupp-bootloader`, `stm32flash -g 0x08002000` also starts it.

Do not write the `standalone` image through a bootloader.
It is linked at `0x08000000`, so it does not start behind a bootloader.
Do not write the `app-10k` image behind `openmupp-bootloader`, or the `app-8k` image behind the legacy bootloader.
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
The build calls `vsl_stamp_image()` from the bootloader submodule (`cmake/VslStampImage.cmake`) for the `app-8k` and `app-10k` images.
It writes the header into the `.elf` file, and it writes the `.hex` and `.bin` files.
The legacy bootloader ignores the header.
The `standalone` image has no header.

To check an image, use the stamp tool with the start address:

```sh
dependencies/openmupp-bootloader/tools/vsl-stamp-image.py --check openmupp-app-8k.bin --base 0x08002000
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
The bootloader uses the start of the flash, and the application uses the rest:

| Image | Bootloader | Application | Size limit | Size limit, multi image or RISC-V bridge |
|---|---|---|---|---|
| `standalone` | none | `0x08000000` | 64 KB | 63 KB |
| `app-8k` | `openmupp-bootloader`, 8 KB | `0x08002000` | 56 KB | 55 KB |
| `app-10k` | legacy Versaloon bootloader, 10 KB | `0x08002800` | 54 KB | 53 KB |

The limits are hard limits for all boards.
The firmware must also fit behind the legacy bootloader, because the existing boards need the `app-10k` image.
Thus CI also checks the `standalone` image of the ST-Link/V2 and of the ST-Link/V2 mini clone against the `app-10k` limit (see [Continuous integration](development.md#continuous-integration)).

- The FLASH region of the link (`__flash_length`) goes from the start of the application to the settings page, or to the end of the 64 KB. With `MUPP_STRICT_SIZE=ON`, the link of an image fails when the application is too large. The CI build jobs set this option.
- Without `MUPP_STRICT_SIZE`, the build skips an image that is too large, and shows a warning (see [Images that do not fit](cmake.md#images-that-do-not-fit)).
- CI reports the size of each image with `ci/check-size.sh` and fails when the `.bin` file is larger than its limit.

The main stack is 2 KB (`__main_stack_size` of the system-stm32 `sections.ld`), because the STM32F103 has only 20 KB RAM.
The link fails when less than the full main stack is free.


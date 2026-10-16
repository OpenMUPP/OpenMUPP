# Firmware update

The Mini and the OtherUse run the firmware behind a bootloader: `versaloon-bootloader` (8 KB) or the legacy Versaloon bootloader (10 KB).
See [Install the bootloader](#install-the-bootloader).
`versaloon-bootloader` is a USB CDC serial port with the ID `0483:5740`.
It uses the STM32 serial bootloader protocol (ST AN3155), so `stm32flash` can write the firmware.

## Start the bootloader

To start `versaloon-bootloader`, use one of these methods:

- Send the Versaloon command `VERSALOON_FW_UPDATE` to interface 0: the two bytes `0x0F 0xAA`. Only the `app-8k` image has this command.
- Hold the key (PB9) down and reset the board.

To start the legacy bootloader, hold the key (PB9) down while you connect the board to USB.

A firmware image built with `VSL_PERSONALITIES=cmsis-dap` has no Versaloon interface.
It has no `VERSALOON_FW_UPDATE` command.
Use the key to start the bootloader.
A firmware image built with `VSL_PERSONALITIES=wch-link` has no `VERSALOON_FW_UPDATE` command either. Use the key.
A multi image in the CMSIS-DAP or the WCH-Link personality has no `VERSALOON_FW_UPDATE` command either.
Change it to the Versaloon personality first, or use the key (see [Multi image](usb-device.md#multi-image)).

A multi image stores its personality in the flash page at `0x0800FC00`.
`stm32flash -w` erases only the pages of the image, so an update keeps the stored personality.
A mass erase of the flash also erases this page.
The firmware then starts with its default personality.

The firmware replies to `VERSALOON_FW_UPDATE` with one status byte:

| Status | Meaning |
|---|---|
| `0x00` | Accepted. The firmware starts the bootloader. |
| `0x01` | Refused. The second byte is not the key `0xAA`, or the image is not `app-8k`. The `standalone` image has no bootloader. The legacy bootloader of the `app-10k` image does not know the boot request. The ST-Link/V2 has only the `standalone` image. The firmware continues to run. |

After the reply, the firmware does these steps:

1. It waits 10 ms, so the host can read the reply. The firmware does not wait for the host to read the reply.
2. It enables the `PWR` and `BKP` clocks and the write access to the backup domain.
3. It writes the boot request `0xB007` to the backup register `BKP->DR1`.
4. It resets the MCU with `NVIC_SystemReset()`. The host sees a USB disconnect.

After the reset, the bootloader finds the boot request, clears it and stays in bootloader mode.
The next reset starts the firmware again.
`src/boot/BootloaderEntry.h` and `src/boot/Bootloader.cpp` have the code.

## Install the bootloader

The Mini and the OtherUse use one of two bootloaders:

- The legacy Versaloon bootloader uses 10 KB. The existing boards have it. It runs the `app-10k` image.
  Hold the key (PB9) down while you connect the board to USB to start it.
  It does not know the boot request, so `VERSALOON_FW_UPDATE` does not start it.
- `versaloon-bootloader` uses 8 KB. It runs the `app-8k` image.
  The submodule `dependencies/versaloon-bootloader` has its source (https://gitlab.vandertogt.org/embedded-projects/versaloon-bootloader).

A board with the legacy bootloader can continue to use it.
To change a board to `versaloon-bootloader`, write the new bootloader one time with a debug probe.
Then write the `app-8k` image, or write the combined image to do both in one step.

The firmware build makes the combined image `versaloon-ng-with-bootloader.hex` (see [Build the firmware](../README.md#build-the-firmware)).
It has the bootloader at `0x08000000` and the `app-8k` image at `0x08002000`.
Write it with a debug probe on the SWD pins of the dongle MCU, for example with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program versaloon-ng-with-bootloader.hex verify reset exit"
```

The build also makes the bootloader image alone, `versaloon-bootloader/versaloon-bootloader-<board>.hex`.
Write it in the same way when the board must keep its firmware.

At its first start, `versaloon-bootloader` write-protects its 8 KB area.
To write a new bootloader over it, remove the write protection first:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "init; reset halt; stm32f1x unlock 0; reset halt; exit"
```

`stm32f1x unlock 0` erases the option bytes and removes all write protection.
The bootloader README has the full rules ("Flash protection").

The legacy bootloader (tag `v1-legacy` of `versaloon-bootloader`) does not set write protection or readout protection.
Thus the debug probe can write the combined image over it without `stm32f1x unlock 0`.
If the write fails, the option bytes of the board were changed: run `stm32f1x unlock 0` first.

## Write the firmware

1. Start the bootloader (see above). On Linux, a new port `/dev/ttyACM*` shows.
   `versaloon-bootloader` has the ID `0483:5740`.
2. Write the `.bin` file of the image for the bootloader:

   ```sh
   # versaloon-bootloader (8 KB)
   stm32flash -w versaloon-ng-app-8k.bin -v -S 0x08002000 /dev/ttyACM0
   # legacy bootloader (10 KB)
   stm32flash -w versaloon-ng-app-10k.bin -v -S 0x08002800 /dev/ttyACM0
   ```

3. Reset the board, or disconnect it and connect it again. The firmware starts.
   With `versaloon-bootloader`, `stm32flash -g 0x08002000` also starts it.

Do not write the `standalone` image through a bootloader.
It is linked at `0x08000000`, so it does not start behind a bootloader.
Do not write the `app-10k` image behind `versaloon-bootloader`, or the `app-8k` image behind the legacy bootloader.
The start address of the image must be the end of the bootloader.

## Application image header

`versaloon-bootloader` starts only an application with a valid header.
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
dependencies/versaloon-bootloader/tools/vsl-stamp-image.py --check versaloon-ng-app-8k.bin --base 0x08002000
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

The OtherUse has an STM32F103C8 with 64 KB flash.
The bootloader uses the start of the flash, and the application uses the rest:

| Image | Bootloader | Application | Size limit | Size limit, multi image |
|---|---|---|---|---|
| `standalone` | none | `0x08000000` | 64 KB | 63 KB |
| `app-8k` | `versaloon-bootloader`, 8 KB | `0x08002000` | 56 KB | 55 KB |
| `app-10k` | legacy Versaloon bootloader, 10 KB | `0x08002800` | 54 KB | 53 KB |

The limits are hard limits for all boards.
The firmware must also fit behind the legacy bootloader, because the existing boards need the `app-10k` image.
Thus CI also checks the `standalone` image of the ST-Link/V2 against the `app-10k` limit.

- `ldscripts/mem-f103.ld` has the FLASH region from the start of the application to the end of the 64 KB. With `VSL_STRICT_SIZE=ON`, the link of an image fails when the application is too large. All presets set this option.
- Without `VSL_STRICT_SIZE`, the build skips an image that is too large, and shows a warning (see [Images that do not fit](development.md#images-that-do-not-fit)).
- CI reports the size of each image with `ci/check-size.sh` and fails when the `.bin` file is larger than its limit.

The main stack is 2 KB (`ldscripts/sections.ld`), because the STM32F103 has only 20 KB RAM.
The link fails when less than the full main stack is free.


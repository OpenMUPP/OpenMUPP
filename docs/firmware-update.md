# Firmware update

The firmware runs behind `versaloon-bootloader`.
The bootloader is a USB CDC serial port with the ID `0483:5740`.
It uses the STM32 serial bootloader protocol (ST AN3155), so `stm32flash` can write the firmware.

## Start the bootloader

Use one of these methods:

- Send the Versaloon command `VERSALOON_FW_UPDATE` to interface 0: the two bytes `0x0F 0xAA`.
- Hold the key (PB9) down and reset the board.

A firmware image built with `VSL_PROTOCOL=cmsis-dap` has no Versaloon interface.
It has no `VERSALOON_FW_UPDATE` command.
Use the key to start the bootloader.

The firmware replies to `VERSALOON_FW_UPDATE` with one status byte:

| Status | Meaning |
|---|---|
| `0x00` | Accepted. The firmware starts the bootloader. |
| `0x01` | Refused. The second byte is not the key `0xAA`, the firmware is linked without the bootloader (`VSL_BOOTLOADER=OFF`), or the board has no `versaloon-bootloader` (ST-Link/V2). The firmware continues to run. |

After the reply, the firmware does these steps:

1. It waits 10 ms, so the host can read the reply. The firmware does not wait for the host to read the reply.
2. It enables the `PWR` and `BKP` clocks and the write access to the backup domain.
3. It writes the boot request `0xB007` to the backup register `BKP->DR1`.
4. It resets the MCU with `NVIC_SystemReset()`. The host sees a USB disconnect.

After the reset, the bootloader finds the boot request, clears it and stays in bootloader mode.
The next reset starts the firmware again.
`src/boot/BootloaderEntry.h` and `src/boot/Bootloader.cpp` have the code.

## Install the bootloader

A new board needs the bootloader one time.
The firmware build makes the bootloader image `versaloon-bootloader/versaloon-bootloader-<board>.hex` (see [Build the firmware](../README.md#build-the-firmware)).
Write it at `0x08000000` with a debug probe on the SWD pins of the dongle MCU, for example with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program versaloon-bootloader/versaloon-bootloader-mini.hex verify reset exit"
```

Then write the firmware through the bootloader (see below).

## Write the firmware

1. Start the bootloader (see above). On Linux, a new port `/dev/ttyACM*` with the ID `0483:5740` shows.
2. Write the `.bin` file of a build with `VSL_BOOTLOADER=ON` at `0x08002800`:

   ```sh
   stm32flash -w versaloon-next-gen.bin -v -S 0x08002800 /dev/ttyACM0
   ```

3. Reset the board, or disconnect it and connect it again. The firmware starts.

Do not write a build with `VSL_BOOTLOADER=OFF` through the bootloader.
That build is linked at `0x08000000`, so it does not start at `0x08002800`.

## Start-up behind the bootloader

The bootloader starts the firmware after a system reset.
Thus all peripherals are in their reset state when the firmware starts.
The bootloader sets `VTOR` to the firmware vector table at `0x08002800`.
The system-stm32 start-up code sets `VTOR` again from the linker script.
The bootloader jumps to the firmware with all interrupts masked (`PRIMASK` set).
`os_startup_initialize_hardware()` clears `PRIMASK` after the pin setup, before the static constructors run.
Without this step, the SysTick and USB interrupts never run, and the host cannot enumerate the device.
The firmware initializes its DATA and BSS sections. It does not use RAM content from the bootloader.

## Flash layout and size limit

`versaloon-bootloader` uses the first 10 KB of flash (`0x08000000` to `0x080027FF`).
The OtherUse has an STM32F103C8 with 64 KB flash.
Thus the application must not be larger than 54 KB.
This is a hard limit for all boards.

- `ldscripts/mem-f103-bootloader.ld` has a 54 KB FLASH region at `0x08002800`. The link fails when the application is too large.
- `ldscripts/mem-f103.ld` has a 64 KB FLASH region at `0x08000000` (`VSL_BOOTLOADER=OFF`).
- CI reports the image size with `ci/check-size.sh` and fails when the `.bin` file is larger than 54 KB.

The main stack is 2 KB (`ldscripts/sections.ld`), because the STM32F103 has only 20 KB RAM.
The link fails when less than the full main stack is free.


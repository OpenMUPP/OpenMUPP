# vsl-ng firmware plan

This document is the plan for the Versaloon next generation firmware (vsl-ng).
The GitLab issues of this project hold the details of each work item.
This document gives the overview: targets, phases, milestones and dependencies.
When an issue and this document do not agree, the issue is correct. Update this document then.

## Targets

- **Versaloon Mini** and **OtherUse**: STM32F103C8 or STM32F103CB, 12 MHz crystal, 72 MHz core clock.
- **ST-Link/V2** clone: STM32F103C8, 8 MHz crystal. It is a compile-only target.
  It is not a supported board now, but a port must be easy.

The firmware runs behind `versaloon-bootloader`.
The bootloader uses the first 10 KB of flash, so the application starts at `0x08002800`.
The OtherUse has an STM32F103C8 (64 KB flash), so the application must not be larger than 54 KB.

The firmware is compatible with the original Versaloon USB protocol.
Unmodified upstream OpenOCD (`interface/vsllink.cfg`) must work with it.
The dongle also has a USB-CDC serial port, which is bridged to USART1.

## Building blocks

| Component | Repository | Use |
|---|---|---|
| system-stm32 | `embedded-libraries/system-stm32` | Start-up code, CMSIS, peripheral drivers (GPIO, USART, ADC, USB device) |
| libArT | `embedded-libraries/libArT` | Core library: streams, logging, USB device stack |
| libart-stm32 | `embedded-libraries/libart-stm32` | STM32 adapters for libArT (serial, USB controller) |

The three libraries are Git submodules in `dependencies/`.
The build system and the CI follow the universal oven controller firmware.

## Phases and milestones

| Phase | Milestone | Result |
|---|---|---|
| A: platform | M1 | system-stm32 supports STM32F1 |
| B: serial | M1: USB-serial adapter | The dongle starts from the bootloader and enumerates as Versaloon plus a COM port. The serial bridge works at all line codings. |
| C: OpenOCD | M2: OpenOCD works | OpenOCD programs and debugs targets over JTAG and SWD |
| D: release | M3: Release | Fast JTAG through SPI and DMA, throughput tuning, documentation and release builds |

## Work items

The estimates are in person-days (low-high).

| Issue | Work item | Estimate | Depends on |
|---|---|---|---|
| #1 | A0: STM32F1 support in system-stm32 (tracking issue) | 9.75-17.75 | system-stm32#9-#16 |
| #2 | B1: Project skeleton: CMake, submodules, CI, linker scripts | 1.5-2 | system-stm32#9 |
| #3 | B7: Board abstraction (Mini, OtherUse, compile-only ST-Link/V2) | 1-1.5 | #2 |
| #4 | B2: Board bring-up: clocks, LEDs, logger, heartbeat | 0.5-1.5 | #3, system-stm32#10, #11 |
| #5 | B3: USB composite device: Versaloon vendor interface and CDC-ACM | 1-2 | #4, system-stm32#15, libart#49 |
| #6 | B5: USB-CDC to UART bridge | 1.5-3 | #5, system-stm32#12, libart#49 |
| #7 | B6: Serial port test script | 0.5-1 | #6 |
| #8 | B8: Bootloader integration and `FW_UPDATE` entry | 1-1.5 | #2, system-stm32#10 |
| #9 | C1: Versaloon command transport and basic commands | 1.5-2 | #2, libart#49 |
| #10 | C2: `USB_TO_ALL` dispatcher, `INFO` and `DELAY` | 1.5-2.5 | #9 |
| #11 | C3: `USB_TO_GPIO` block (reset lines) | 0.5-1 | #10, system-stm32#11 |
| #12 | C4: JTAG raw engine (bit-bang) | 1.5-2.5 | #10, system-stm32#14 |
| #13 | C5: SWD engine | 2-3.5 | #10, system-stm32#14 |
| #14 | C6: Target power control and Vref sense | 0.5-1 | #10, system-stm32#13 |
| #15 | C7: OpenOCD integration test | 2.5-5 | #11, #12, #13, #14 |
| #16 | D1: JTAG fast path with SPI and DMA | 2.5-4 | #12, #15 |
| #17 | D2: USB and protocol throughput tuning | 1-2 | #15 |
| #18 | D3: Documentation and release builds | 1-1.5 | #15, #16 |

## Dependencies

The diagram shows the dependencies between the work items of this project.
Phase A (#1) is a dependency of all phase B items.

```mermaid
flowchart LR
    A0["#1 A0 F1 platform"] --> B1["#2 B1 skeleton"]
    B1 --> B7["#3 B7 boards"]
    B7 --> B2["#4 B2 bring-up"]
    B2 --> B3["#5 B3 USB composite"]
    B3 --> B5["#6 B5 serial bridge"]
    B5 --> B6["#7 B6 serial test"]
    B1 --> B8["#8 B8 bootloader"]
    B1 --> C1["#9 C1 transport"]
    C1 --> C2["#10 C2 USB_TO_ALL"]
    C2 --> C3["#11 C3 GPIO"]
    C2 --> C4["#12 C4 JTAG raw"]
    C2 --> C5["#13 C5 SWD"]
    C2 --> C6["#14 C6 power"]
    C3 --> C7["#15 C7 OpenOCD test"]
    C4 --> C7
    C5 --> C7
    C6 --> C7
    C4 --> D1["#16 D1 JTAG SPI+DMA"]
    C7 --> D1
    C7 --> D2["#17 D2 throughput"]
    C7 --> D3["#18 D3 docs, release"]
    D1 --> D3
```

## Hardware tests

Issues with the label `needs-hardware` need a Versaloon Mini or an OtherUse for the acceptance test.
Do the software part first and verify it with the build and the host unit tests.
Then record the open hardware test in the issue.

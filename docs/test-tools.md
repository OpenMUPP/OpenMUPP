# Test tools

The scripts in `tools/hwtest/` and `tools/analysis/` are for the development and the test of the firmware.
The host tools that you use with the probe are in the [README](../README.md#tools).
The scripts need Python 3.
Each script is executable. On Windows, start it with `py`.

The folders have these parts:

- `tools/hwtest/`: the hardware tests and benchmarks. They need a probe, and most of them need a target.
- `tools/analysis/`: the decoders of logic analyzer exports and USB captures (`usbpcap/`).
- `tools/tests/`: the unit tests of the scripts. They need no hardware.

| Script | Function | Needs |
|---|---|---|
| `tools/hwtest/serial_loopback_test.py` | Tests the UART bridge with a loopback wire from TX to RX (see [uart-bridge.md](uart-bridge.md)). | `pyserial` |
| `tools/hwtest/dap_uart_loopback_test.py` | Tests the `DAP_UART_*` commands of the CMSIS-DAP personality with a loopback wire from TX to RX (see [cmsis-dap.md](cmsis-dap.md#uart-hardware-test)). | `pyusb`, `pyserial` for `--serial-port` |
| `tools/hwtest/avr_data_break_test.py` | Sets the AVR JTAG data breakpoints with the EDBG commands and shows the PC and the cause of each break (see [avr.md](avr.md#jtag-data-breakpoints)). | `pyusb` |
| `tools/hwtest/flash_benchmark.py` | Measures the flash speed with OpenOCD (see [development.md](development.md#flash-speed-benchmark)). | OpenOCD |
| `tools/hwtest/pyocd_jtag_test.py` | Runs the pyOCD JTAG test on an STM32F1 target with the CMSIS-DAP personality (see [cmsis-dap.md](cmsis-dap.md#jtag-hardware-test)). `--protocol swd` runs the same checks on SWD. `--target` selects the pyOCD target. It erases the flash of the target. | pyOCD |
| `tools/hwtest/probe_hw_test.py` | Runs the hardware test of a multi image on an STM32F1 target and a UART loopback. It tests the Versaloon personality with OpenOCD (SWD and JTAG). It then switches to the CMSIS-DAP personality and tests it with OpenOCD, pyOCD and probe-rs. It also runs the UART loopback tests. It erases the flash of the target. | `pyusb`, `pyserial`, OpenOCD, pyOCD, probe-rs (`--skip` for a missing tool) |
| `tools/analysis/wch_link_frames_2wire.py`, `tools/analysis/wch_link_frames_1wire.py` | Decode the wire frames of a WCH-LinkE from a logic analyzer CSV file: 54-clock frames on two wires, 41-pulse frames on one wire (see [wch-link-protocol.md](wch-link-protocol.md#wire-frame-format)). | none |
| `tools/analysis/clock_rate.py` | Measures the clock rate of a bit-bang engine from a logic analyzer CSV file (see [clock-pacing.md](clock-pacing.md#measurements)). | none |
| `tools/analysis/swd_timing.py` | Measures the SWCLK half periods, the SWDIO setup time and the gaps between transfers from a logic analyzer CSV file (see [clock-pacing.md](clock-pacing.md#measurements)). | none |

The unit tests in `tools/tests/` run with this command:

```sh
python3 -m unittest discover -s tools -p "test_*.py"
```

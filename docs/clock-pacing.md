# Clock pacing

This note gives the decision for the clock rate of the clocked bit-bang engines (#78), and the measurements on the wire.
The engines are:

- SWD: `src/swd/SwdBitBang.h`
- JTAG bit-bang: `src/jtag/JtagBitBang.h` (the JTAG fast path uses SPI and DMA, see [JTAG fast path](usb-device.md#jtag-fast-path))
- RVSWD, the two-wire interface of the WCH-Link personality: `src/rvswd/RvSwdEngine.h`

The one-wire (SDI) engine uses a hardware timer and DMA (see [wch-link.md](wch-link.md)). This note does not apply to it.

## Problem

In a clocked protocol, the target samples on a clock edge.
Code jitter changes only the clock phases, so the data stays correct while the frequency and the setup and hold times are in the limits.
But the rate on the wire must agree with the requested rate.

Before this change, each half period waited a number of core cycles from a time stamp near its edge.
Thus the code of each half period added to the period:

- SWD and RVSWD subtracted an estimate of the loop code, `loopOverheadCycles = 24`.
  This value was measured only for SWD at `delay` 1 on the Versaloon Mini.
  On the OtherUse, some SWD periods were shorter than requested, and RVSWD was up to 4.8 % faster than requested.
- JTAG did not subtract the loop code. TCK was 6 % to 40 % slower than requested.

The error changes with the core clock, the flash wait states and the compiler output.

## Decision

Each edge comes at a deadline of the core cycle counter (DWT `CYCCNT`): the last deadline plus the half period.
`src/ClockPacer.h` has the pacer. The engines use it for each edge:

```cpp
pacer.wait(pins);    // until the deadline
pins.swclk(level);   // the edge
pacer.edge(pins);
```

The code between two edges runs inside the half period.
Thus the mean rate does not depend on the code, while the code of a half period is shorter than the half period.

An edge more than `pacing::lateSlackCycles` (32) cycles after its deadline is late.
This occurs when the code took the whole half period (for example a call of the background service), or when an interrupt came between the wait and the edge.
The next half period then starts at the edge.
Thus a late edge makes its own half period longer, and the clock does not catch up with shorter half periods.

The slack must be more than the normal time from a deadline to the counter read in `edge()`.
On the STM32F103 at 72 MHz, this time is up to about 20 cycles: one pass of the wait loop, the pin write, and the counter read.
An interrupt adds at least its entry and exit (24 cycles on the Cortex-M3), so the pacer sees each interrupt in this window.
An edge within the slack makes the next half period shorter by up to 32 cycles.

The edges have the jitter of the wait loop: up to one pass of it.
Thus single periods are up to about 12 % shorter or longer than requested at 1 MHz, but the mean is correct.

### Rejected options

- A timer that paces the clock, with DMA, as the one-wire engine does.
  SWD must read the ACK in the middle of a transfer and act on it (OK, WAIT, FAULT): a DMA table that is ready before the transfer does not fit.
  On the ST-Link/V2, SWCLK (PA5) has no timer channel.
  A DMA transfer takes 80 to 210 ns on the OtherUse (#71), and this must fit in the shortest clock phase.
- SPI for the data, as the JTAG fast path does.
  SWD needs SWDIO on MOSI and MISO, and SPI moves 8 bits at a time. The turnaround (1 bit) and the ACK (3 bits) do not fit this.
  Not all boards have the necessary wiring.
- Measured loop constants for each board and build.
  This is the state before this change. Each change of the code or the compiler needs a new measurement.

## Limits

- The timed rate is limited by the code of the half periods.
  The pacer adds about 10 to 15 cycles to each edge.
  On the STM32F103, timed SWD thus gives at most about 1 MHz: a request of 1.8 MHz gives 0.96 MHz (before this change: 1.79 MHz).
  The clock is never faster than the request. Issue #117 tracks a faster mode.
- The untimed SWD engine (`delay` 0, or 1.85 MHz and more in CMSIS-DAP) does not use the pacer.
  Its rate depends on the code: 2.5 MHz before this change and 2.7 MHz after it, on the OtherUse.
  This is faster than the 1.85 MHz of the table. Issue #118 tracks this.
- At 1 MHz, the JTAG engine makes one longer half period at each byte (the code of the next byte is longer than 36 cycles).
  The median period is correct, the mean rate is 92 % of the request.
- Only the OtherUse is measured. The Mini has the same MCU, core clock and pins.
  The ST-Link/V2 and the STM32H5 boards are not measured.

## Measurements

### Setup

- Probe: OtherUse (STM32F103C8 at 72 MHz), the `otheruse-triple` image with `-DVSL_WCH_LINK_WCH_USB_ID=ON`, `app-8k`.
  "Before" is `dev` at `07d900e`. "After" is this change at `6e7ca26`.
- Target: WeAct STM32H523 on SWD. JTAG and RVSWD had no target: the probe clocks the frames also without a reply.
- Logic analyzer: Saleae Logic Pro 8 at 100 MS/s (10 ns resolution), SWDIO on channel 4, SWCLK and TCK on channel 5.
- Traffic:
  - SWD: `probe-rs read --chip STM32H523CC --protocol swd --speed <kHz> b32 0x08000000 64` (CMSIS-DAP personality)
  - JTAG: `openocd -c "adapter driver cmsis-dap" -c "transport select jtag" -c "adapter speed <kHz>" -c init -c shutdown`.
    Below 1125 kHz, the OtherUse uses the JTAG bit-bang engine.
  - RVSWD: `wlink status --chip CH32V20X --speed low|high` (WCH-Link personality)

`tools/clock_rate.py` calculates the rate from a raw CSV export of Logic 2.
It counts only runs of 30 or more consecutive bits with periods below 1.3 times the nominal period.
Thus the gaps between transfers, and the longer phases of an RVSWD frame, are not part of the rate.
It also prints the median and the shortest of all periods (gaps included), also when there is no run.

```sh
python3 tools/clock_rate.py digital.csv 1 1000
```

The nominal period is two half periods, each rounded up to full core cycles.
For example, 235 kHz gives 154 cycles for each half period: 4277.8 ns.

### Results

The rate is the mean over the runs. "Shortest" is the shortest single period in the runs.

| Engine | Request | Nominal period | Before | After | Shortest before | Shortest after |
|---|---|---|---|---|---|---|
| SWD | 100 kHz | 10000 ns | 99.5 kHz (−0.46 %) | 99.8 kHz (−0.16 %) | 9870 ns | 9840 ns |
| SWD | 235 kHz | 4277.8 ns | 233.6 kHz (−0.07 %) | 233.6 kHz (−0.06 %) | 4100 ns | 4100 ns |
| SWD | 500 kHz | 2000 ns | 495.3 kHz (−0.94 %) | 498.3 kHz (−0.33 %) | 1880 ns | 1810 ns |
| SWD | 1000 kHz | 1000 ns | 966.9 kHz (−3.31 %) | 995.6 kHz (−0.44 %) | 940 ns | 880 ns |
| SWD | 1800 kHz | 555.6 ns | 1788.9 kHz (−0.61 %) | 961.5 kHz (median) | 520 ns | 720 ns |
| JTAG | 100 kHz | 10000 ns | 94.0 kHz (−6.02 %) | 100.0 kHz (−0.01 %) | 10550 ns | 9960 ns |
| JTAG | 500 kHz | 2000 ns | 377.4 kHz (median) | 498.4 kHz (−0.33 %) | 2540 ns | 1970 ns |
| JTAG | 1000 kHz | 1000 ns | 606.1 kHz (median) | 1000.0 kHz (median) | 1650 ns | 990 ns |
| RVSWD | low (478 kHz) | 2111.1 ns | 478.2 kHz (+0.94 %) | 472.6 kHz (−0.24 %) | 2020 ns | 2000 ns |
| RVSWD | high (907 kHz) | 1111.1 ns | 943.4 kHz (+4.83 %) | 897.5 kHz (−0.28 %) | 970 ns | 1050 ns |

"Median" is the median of all periods, and "Shortest" in these rows is the shortest of all periods: before this change, JTAG at 500 and 1000 kHz had no period below 1.3 times the nominal period. SWD at 1800 kHz after this change is limited by the code (see [Limits](#limits)).
The untimed SWD engine gave a median period of 400 ns (2.50 MHz) before, and 370 ns (2.70 MHz) after this change.

Findings:

- Up to 1 MHz, all engines are within 0.5 % of the request, and the mean rate is never faster than the request.
  Before this change, the error was −6 % to −40 % for JTAG and up to +4.8 % for RVSWD.
- The single periods spread by the jitter of the wait loop. At 1 MHz, the shortest SWD period is 880 ns.
- The first version of the pacer had a smaller slack (16 cycles, then 12 cycles from the end of the wait), and more state for each edge.
  On the OtherUse, the normal delay was often above the slack, so the pacer started the deadlines again at many edges: SWD was 2 % slower at 100 kHz and 14 % slower at 235 kHz.
  The state for each edge made the code of a half period at 1 MHz longer than 36 cycles: SWD was 41 % slower.
  Measure the rate again after a change of the engines, the pacer or the core clock.

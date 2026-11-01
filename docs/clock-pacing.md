# Clock pacing

This document describes how the clocked bit-bang engines time their clock, and why.
It also gives the measurements of the clock on the wire.
The engines are:

- SWD: `src/swd/SwdBitBang.h`
- JTAG bit-bang: `src/jtag/JtagBitBang.h` (the JTAG fast path uses SPI and DMA, see [JTAG fast path](usb-device.md#jtag-fast-path))
- RVSWD, the two-wire interface of the WCH-Link personality: `src/rvswd/RvSwdEngine.h`

The one-wire (SDI) engine uses a hardware timer and DMA (see [wch-link.md](wch-link.md)). This document does not apply to it.

## Requirement

In a clocked protocol, the target samples on a clock edge.
Code jitter changes only the clock phases, so the data stays correct while the frequency and the setup and hold times are in the limits.
But the rate on the wire must agree with the requested rate.
The rate must never be faster than the request, because a target can have a clock limit.

A design that adds the time of the code to each half period does not meet this requirement.
The time of the code changes with the core clock, the flash wait states and the compiler output.

## Design

Each edge comes at a deadline of the core cycle counter (DWT `CYCCNT`): the last deadline plus the half period.
`src/ClockPacer.h` has the pacer. The engines use it for each edge:

```cpp
pacer.wait(pins);    // until the deadline
pins.swclk(level);   // the edge
pacer.edge(pins);
```

The code between two edges runs inside the half period.
Thus the mean rate does not depend on the code, while the code of a half period is shorter than the half period.

### Late edges

An edge more than `pacing::lateSlackCycles` cycles after its deadline is late: 32 cycles on the STM32F1, 16 cycles on the STM32H5 (see [STM32H5](#stm32h5)).
This occurs when the code took the whole half period (for example a call of the background service), or when an interrupt came between the wait and the edge.
The next half period then starts at the edge.
Thus a late edge makes its own half period longer, and the clock does not catch up with shorter half periods.
This is the reason why the mean rate is never faster than the request.

The slack must be more than the normal time from a deadline to the counter read in `edge()`.
On the STM32F103 at 72 MHz, this time is up to about 20 cycles: one pass of the wait loop, the pin write, and the counter read.
An interrupt adds at least its entry and exit (24 cycles on the Cortex-M3), so the pacer sees each interrupt in this window.
An edge within the slack makes the next half period shorter by up to 32 cycles.

The edges have the jitter of the wait loop: up to one pass of it.
Thus single periods are shorter or longer than requested, but the mean is correct.
On the Mini, the shortest SWD period at 1 MHz was 880 ns with the member pacer and 846 ns with the local pacer (15 % shorter than the nominal 1000 ns).
The two measurements have different setups (sample rate and target), so a part of the difference is not from the code.

### Local pacer

In the SWD and JTAG engines, the pacer is a local variable of one engine operation, not a member of the engine.
Each SWD operation (a transfer or a sequence) has its own pacer, as each JTAG operation has.
The RVSWD engine keeps the pacer as a member.
The reason is the compiler: it assumes that a store to a GPIO register can change a `uint32_t` member.
Thus it loads and stores the deadline of a member pacer at each edge.
A local pacer keeps the deadline in a register.
The first half period of each operation is a full half period.
See [SWD pacer in registers](#swd-pacer-in-registers) for the effect on the rate.

### Alternatives

The design does not use these options:

- A timer that paces the clock, with DMA, as the one-wire engine does.
  SWD must read the ACK in the middle of a transfer and act on it (OK, WAIT, FAULT): a DMA table that is ready before the transfer does not fit.
  On the ST-Link/V2, SWCLK (PA5) has no timer channel.
  A DMA transfer takes 80 to 210 ns on the Mini, and this must fit in the shortest clock phase.
- SPI for the data, as the JTAG fast path does.
  SWD needs SWDIO on MOSI and MISO, and SPI moves 8 bits at a time. The turnaround (1 bit) and the ACK (3 bits) do not fit this.
  Not all boards have the necessary wiring.
- Measured loop constants for each board and build.
  In this method, each half period waits a number of core cycles from a time stamp near its edge, minus a constant for the loop code.
  Each change of the code or the compiler needs a new measurement.
  The method was measured only for SWD at `delay` 1 on the Mini (`loopOverheadCycles = 24`).
  There, some SWD periods were shorter than requested, and RVSWD was up to 4.8 % faster than requested.
  JTAG did not subtract the loop code: TCK was 6 % to 40 % slower than requested.

## Limits

- The timed rate is limited by the code of the half periods.
  The pacer adds about 10 to 15 cycles to each edge.
  On the STM32F103, the code of a timed SWD half period is about 23 to 28 cycles.
  Thus a request of 1.8 MHz gives 1.3 to 1.56 MHz.
  The clock is never faster than the request. See [SWD pacer in registers](#swd-pacer-in-registers).
- The untimed SWD engine (`delay` 0, or 3 MHz and more in CMSIS-DAP) does not use the pacer.
  Its rate depends on the code: 2.42 to 2.7 MHz on the Mini.
  At `delay` 0, this is faster than the 1.85 MHz of the table.
  In CMSIS-DAP, the engine runs untimed only from `swd::maxClockHz` (3 MHz), so a request does not give a faster clock.
  Only a slow core uses the untimed engine (`swd::runsUntimed()`): on the STM32H5, it gave 13 MHz, so the H5 is timed at all clocks.
- At 1 MHz, the JTAG engine makes one longer half period at each byte (the code of the next byte is longer than 36 cycles).
  The median period is correct, the mean rate is 92 % of the request.
- The Mini and the WeAct STM32H523 are measured.
  The ST-Link/V2 is not measured.

## Measurements

### Setup

- Probe: Mini (STM32F103C8 at 72 MHz), an image with `MUPP_PERSONALITIES=versaloon,cmsis-dap,wch-link` and `-DMUPP_WCH_LINK_WCH_USB_ID=ON`, `app-8k`.
  "Loop constant" is the method with a measured loop constant (see [Alternatives](#alternatives)). "Pacer" is this design with the SWD pacer as a member of the engine (see [SWD pacer in registers](#swd-pacer-in-registers) for the local pacer).
- Target: WeAct STM32H523 on SWD. JTAG and RVSWD had no target: the probe clocks the frames also without a reply.
- Logic analyzer: Saleae Logic Pro 8 at 100 MS/s (10 ns resolution), SWDIO on channel 4, SWCLK and TCK on channel 5.
- Traffic:
  - SWD: `probe-rs read --chip STM32H523CC --protocol swd --speed <kHz> b32 0x08000000 64` (CMSIS-DAP personality)
  - JTAG: `openocd -c "adapter driver cmsis-dap" -c "transport select jtag" -c "adapter speed <kHz>" -c init -c shutdown`.
    Below 1125 kHz, the Mini uses the JTAG bit-bang engine.
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

The rate is the mean over the runs. The percentage is against the nominal period, not against the request. "Shortest" is the shortest single period in the runs.

| Engine | Request | Nominal period | Loop constant | Pacer | Shortest, loop constant | Shortest, pacer |
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

"Median" is the median of all periods, and "Shortest" in these rows is the shortest of all periods: with the loop constant, JTAG at 500 and 1000 kHz had no period below 1.3 times the nominal period. SWD at 1800 kHz with the pacer is limited by the code (see [Limits](#limits)).
The untimed SWD engine gave a median period of 400 ns (2.50 MHz) with the loop-constant build, and 370 ns (2.70 MHz) with the pacer build.

Findings:

- Up to 1 MHz, all engines are within 0.5 % of the request, and the mean rate is never faster than the request.
  With the loop constant, the error was −6 % to −40 % for JTAG and up to +4.8 % for RVSWD.
- The single periods spread by the jitter of the wait loop. At 1 MHz, the shortest SWD period was 880 ns in this measurement.
- A first version of the pacer had a smaller slack (16 cycles, then 12 cycles from the end of the wait), and more state for each edge.
  On the Mini, the normal delay was often above the slack, so the pacer started the deadlines again at many edges: SWD was 2 % slower at 100 kHz and 14 % slower at 235 kHz.
  The state for each edge made the code of a half period at 1 MHz longer than 36 cycles: SWD was 41 % slower.
  Thus the slack and the state for each edge matter. Measure the rate again after a change of the engines, the pacer or the core clock.

### SWD pacer in registers

With the pacer as a member of the SWD engine, timed SWD on the STM32F103 was limited to about 1 MHz.
The compiler loaded and stored the deadline at each edge (see [Local pacer](#local-pacer)).
This section compares the member pacer with the local pacer.

Measured on 2026-10-26 with the `mini-cmsis-dap` image (`app-8k`) on the Mini, and an STM32F103ZET6 as the target.
The Saleae Logic Pro 8 ran at 500 MS/s, with SWCLK on channel 6 and a glitch filter of 20 ns.
Each run read 1 KiB of the target flash with `probe-rs read --chip STM32F103ZE --protocol swd --speed <kHz>`.
"Member" is the member pacer, "Local" is the local pacer. The rate is the mean over the runs, against the nominal period.

| Request | Member | Local | Shortest period, local |
|---|---|---|---|
| 100 kHz | 99.8 kHz (−0.16 %) | 99.9 kHz (−0.07 %) | 9722 ns |
| 500 kHz | 498.9 kHz (−0.22 %) | 499.6 kHz (−0.09 %) | 1778 ns |
| 1000 kHz | 988.0 kHz (−1.20 %) | 999.8 kHz (−0.02 %) | 846 ns |
| 1200 kHz | 1199.2 kHz (−0.07 %) | 1198.6 kHz (−0.11 %) | 734 ns |
| 1500 kHz | 1000.0 kHz (median) | 1495.4 kHz (−0.31 %) | 610 ns |
| 1800 kHz | 972.8 kHz (median) | 1562.5 kHz (median) | 568 ns |

At 1800 kHz, the nominal period is 40 core cycles. Most periods with the local pacer are 46 cycles, some are 57 cycles: the code limits the clock.
The shortest single period is 41 cycles, so the clock is not faster than the request.

The code layout in the flash changes the code-limited rates.
In a second build with only other constants, a request of 1800 kHz gave 1.31 MHz (median period 764 ns), and the untimed engine gave 2.67 MHz.
A third build gave the values of the table again, and 2.45 MHz untimed.
The cause is probably the flash wait states and the alignment of the loops. This is not confirmed.

Thus CMSIS-DAP runs the untimed engine only from 3 MHz (`swd::maxClockHz`).
A lower limit of 1.85 MHz gave 2.4 to 2.7 MHz for a request of 1.85 or 2 MHz: faster than the request.
Measured with the third build:

| Request | Engine | Rate |
|---|---|---|
| 1850 kHz | timed | 1562.5 kHz (median), shortest period 568 ns |
| 2500 kHz | timed | 1562.5 kHz (median), shortest period 568 ns |
| 2999 kHz | timed | 1562.5 kHz (median), shortest period 568 ns |
| 3000 kHz | untimed | 2445.0 kHz (mean of the runs), shortest period 346 ns |
| 4000 kHz | untimed | 2325.6 kHz (median), shortest period 346 ns |

## STM32H5

This section gives the measurements on the STM32H5 at 250 MHz.
The SWD rows use the member pacer, except in the sections that name the local pacer.

### Setup

- Probe: WeAct STM32H523 (STM32H523CCU6 at 250 MHz), an image with the personalities `versaloon,cmsis-dap` (the `weacth523-dual` preset of that time).
- Target: STM32F103ZET6 on the 20-pin JTAG header, at its reset clock (8 MHz HSI).
- Logic analyzer: Saleae Logic Pro 8 at 500 MS/s (2 ns resolution), TMS and SWDIO on channel 4, TCK and SWCLK on channel 5.
  A glitch filter of 20 ns is on channel 5.
  Without it, the analyzer saw pulses of 6 ns on SWCLK at each rising edge of SWDIO: crosstalk in the wires to the analyzer.
- Traffic:
  - SWD: `probe-rs read --chip STM32F103ZE --protocol swd --speed <kHz> b32 0x08000000 256`
  - JTAG: `openocd -c "adapter driver cmsis-dap" -c "transport select jtag" -f target/stm32f1x.cfg -c "adapter speed <kHz>" -c "reset_config trst_and_srst" -c init -c "mdw 0x08000000 256" -c shutdown`.
    Set `adapter speed` after `target/stm32f1x.cfg`: the file sets 1000 kHz.
    After an SWD session, the STM32F103 stays in SWD mode. A pyOCD connect with `-O dap_protocol=jtag` sends the SWD-to-JTAG sequence.

All reads gave the same data at all clocks.
The STM32F103 permits TCK up to F_CPU / 6 (1.33 MHz at 8 MHz), so the JTAG reads at 4 MHz were out of its specification. They passed in this test.

### Results

The nominal period is two half periods, each rounded up to full core cycles (4 ns).

| Engine | Request | Nominal period | Rate (mean of the runs, against the nominal period) | Median of all periods | Shortest |
|---|---|---|---|---|---|
| SWD | 100 kHz | 10000 ns | 100.0 kHz (−0.01 %) | 10000 ns | 9972 ns |
| SWD | 235 kHz | 4256 ns | 234.7 kHz (−0.09 %) | 4260 ns | 4228 ns |
| SWD | 1000 kHz | 1000 ns | 998.6 kHz (−0.14 %) | 1000 ns | 978 ns |
| SWD | 1800 kHz | 560 ns | 1779.7 kHz (−0.34 %) | 564 ns | 538 ns |
| SWD | 4000 kHz | 256 ns | 3879.1 kHz (−0.69 %) | 262 ns | 234 ns |
| SWD | 12000 kHz | 88 ns | no run | 180 ns (5.6 MHz) | 152 ns |
| JTAG | 100 kHz | 10000 ns | 99.9 kHz (−0.12 %) | 9998 ns | 9982 ns |
| JTAG | 235 kHz | 4256 ns | 235.0 kHz (−0.00 %) | 4260 ns | 4234 ns |
| JTAG | 1000 kHz | 1000 ns | 1000.2 kHz (+0.02 %) | 1004 ns | 980 ns |
| JTAG | 4000 kHz | 256 ns | no run | 298 ns (3.4 MHz) | 250 ns |
| JTAG | 12000 kHz | 88 ns | no run | 204 ns (4.9 MHz) | 202 ns |

"No run": the code makes the periods longer than 1.3 times the nominal period. The code then limits the clock, and the clock is slower than the request.
The JTAG row at 1000 kHz is +0.02 %: this is in the resolution of the analyzer (2 ns in a period of 1000 ns).

Findings:

- Up to 4 MHz, the mean SWD rate is within 0.7 % of the nominal period, and never faster than the request.
  The rounding to full core cycles makes the nominal rate lower than the request: 2.3 % below at 4 MHz (3906 kHz), 0.8 % below at 1800 kHz (1786 kHz).
  Up to 1 MHz, JTAG is within 0.15 % of the request.
- The code of a timed SWD half period is about 22 cycles: a request of 12 MHz gives 5.6 MHz.
  This is with the member pacer. See [SWD pacer on the STM32H5](#swd-pacer-on-the-stm32h5) for the local pacer.
- The untimed SWD engine of the first test gave 13 MHz (median period 76 ns) at a request of 4 MHz.
  Thus the H5 does not use it (`swd::runsUntimed()`): the timed engine makes all clocks, also at `delay` 0 (1.85 MHz).
- With a slack of 32 cycles, JTAG at 4 MHz had high half periods of 60 ns and periods of 178 ns (5.6 MHz).
  The JTAG code is longer than a half period of 32 cycles at some edges. An edge late by less than the slack then made the next half period shorter.
  With a slack of 16 cycles, the shortest period at 4 MHz is 250 ns. The rates up to 1 MHz did not change.
- The JTAG bit-bang engine gives about 3.4 MHz at a request of 4 MHz, and 4.9 MHz at most.
  `jtag::bitBangMaxKHz` is only for the choice between the bit-bang engine and the SPI fast path. See [STM32H5 JTAG fast path](#stm32h5-jtag-fast-path) for the STM32H5 value.
- RVSWD on the STM32H5 is not measured.

### SWD pacer on the STM32H5

Measured on 2026-10-26 with the setup above, with the `weacth523-cmsis-dap` image.
The Saleae Logic Pro 8 had SWCLK on channel 7, with a glitch filter of 20 ns.
"Member" is the member pacer, "Local" is the local pacer.
The rate is the mean over the runs, against the nominal period. "Median" is the median of all periods.

| Request | Nominal period | Member | Local | Shortest period, local |
|---|---|---|---|---|
| 1000 kHz | 1000 ns | 998.9 kHz (−0.11 %) | 999.7 kHz (−0.03 %) | 968 ns |
| 2000 kHz | 504 ns | 1978.5 kHz (−0.28 %) | 1983.5 kHz (−0.03 %) | 472 ns |
| 4000 kHz | 256 ns | 3882.8 kHz (−0.60 %) | 3900.5 kHz (−0.15 %) | 232 ns |
| 5000 kHz | 200 ns | 4964.3 kHz (−0.71 %) | 4998.7 kHz (−0.03 %) | 180 ns |
| 6000 kHz | 168 ns | 5928.7 kHz (−0.40 %) | 5963.7 kHz (+0.19 %) | 134 ns |
| 8000 kHz | 128 ns | 4807.7 kHz (median) | 6024.1 kHz (median) | 134 ns |
| 10000 kHz | 104 ns | 5555.6 kHz (median) | 5208.3 kHz (median) | 136 ns |
| 20000 kHz | 56 ns | 6410.3 kHz (median) | 6097.6 kHz (median) | 136 ns |

Findings:

- Up to 6 MHz, the mean rate with the local pacer is within 0.2 % of the nominal period, and the mean is never faster than the request.
  Below the code limit, the error is smaller than with the member pacer: −0.15 % instead of −0.60 % at 4 MHz.
- At 6 MHz, the mean is 0.19 % above the nominal period (5952 kHz), but below the request.
  This comes from the filter of `tools/clock_rate.py`: it drops the long periods after a late edge, and keeps the shorter periods that follow.
- Single periods at 5 and 6 MHz are often shorter than the request.
  At 5 MHz, the code is almost a full half period (24 of 25 cycles).
  Thus many edges are late within the slack, and the next half period is then shorter (see [Late edges](#late-edges)).
  With the local pacer, 65 % of the periods at 5 MHz and 47 % at 6 MHz are shorter than the request. With the member pacer, these values were 16 % and 23 %.
  At 5 MHz, the median period is 192 ns (5.21 MHz). At 6 MHz, the shortest period is 134 ns (7.5 MHz, 20 % short).
- From 8 MHz, the code limits the clock to 5.2 to 6.1 MHz (median). With the member pacer, it was 4.8 to 6.4 MHz.
  The limit does not increase with the request: 8 MHz gave 6.0 MHz, but 10 MHz gave 5.2 MHz.
  The code limit on the STM32H5 changed less than on the STM32F103, and at 10 MHz it is lower than with the member pacer.
- All requests used the timed engine. All reads were correct.

### STM32H5 JTAG fast path

Measured on 2026-10-25 with the setup above, with the `cmsis-dap` personality of the `weacth523-dual` image.
The target ran at 64 MHz (`tools/stm32f1_clock.tcl`).
Each run read 8 KiB of the target flash with OpenOCD. All reads gave the same data.

| Request | Engine | Measured TCK |
|---|---|---|
| 1000 kHz | bit-bang | 1000 kHz (periods of 992 ns and 1016 ns) |
| 2000 kHz | bit-bang | 1983 kHz (−0.85 %) |
| 3000 kHz | bit-bang | 2874 kHz (−4.2 %) |
| 3900 kHz | bit-bang | 3564 kHz (−8.6 %) |
| 4000 kHz | SPI | 3906 kHz, periods 254 ns to 258 ns |
| 8000 and 10000 kHz | SPI | 7812 kHz, period 128 ns |

The bit-bang rates at 2000 kHz to 3900 kHz are higher than the SPI speed of 1953 kHz.
Thus `jtag::bitBangMaxKHz` is 3000 on the STM32H5.
The shortest single bit-bang period at 3900 kHz was 246 ns, as in the bit-bang results above.
The SPI speed of 15.6 MHz is not measured: the STM32F103 permits TCK only up to F_CPU / 6 (10.6 MHz at 64 MHz).
Thus the firmware does not use 15.6 MHz (`jtag::spiMinShift` is 4 on the STM32H5).


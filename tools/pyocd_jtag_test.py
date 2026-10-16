#!/usr/bin/env python3
"""pyOCD JTAG (or SWD) test of an STM32F1 target on the CMSIS-DAP personality.

pyOCD 0.45.1 sends DAP_JTAG_Configure with one device of IR length 4. The
STM32F1 has two TAPs: the Cortex-M3 DAP (IR length 4, next to TDO) and the
boundary-scan TAP (IR length 5). Thus the script patches the pyOCD protocol
class to configure the chain with the IR lengths 4 and 5. pyOCD does not
scan for the TAPs: the chain is set by the script.

The script checks, in this order:
  - JTAG only: DAP_JTAG_IDCODE of device 0, and the error replies for
    device 2 and for bad DAP_JTAG_Configure values (9 devices, IR length 0,
    IR length 33);
  - flash, read back (compared in Python) and reset and halt;
  - step, hardware breakpoint, resume and halt;
  - RAM write and read (4 words and an 8 KiB block).

The flash image has 32 KiB: a vector table (SP = 0x20005000, reset vector
0x08000041), the loop `adds r1, #1` / `b` back to the `adds` at 0x08000040
(halfwords 0x3101 0xe7fd), and pseudo-random data up to 32 KiB.

Above 1000 kHz the script sets the core clock of the target to 64 MHz
(HSI / 2 x 16) first, because TCK must not be above F_CPU / 6. The target
clock goes back to 8 MHz at each reset.

The script erases the flash of the target.

With --protocol swd, the script does the same checks on SWD, without the
JTAG chain patch and the DAP_JTAG_* checks.

The default target is the built-in pyOCD target stm32f103rc (2 KiB flash
pages). For a low or medium density STM32F103 (1 KiB pages), select the
target of the chip with --target, for example stm32f103c8 (needs the
Keil.STM32F1xx_DFP pack).

Usage:
    python3 pyocd_jtag_test.py [TCK in Hz, default 1000000]
    python3 pyocd_jtag_test.py 4000000 --protocol swd --target stm32f103c8
"""
import argparse
import os
import random
import struct
import sys
import tempfile
import time

from pyocd.probe.pydapaccess import cmsis_dap_core as core

CHAIN_IR_LENGTHS = [4, 5]  # device 0 = CPU DAP (next to TDO), device 1 = boundary scan
_orig_jtag_configure = core.CMSISDAPProtocol.jtag_configure

from pyocd.core.helpers import ConnectHelper  # noqa: E402
from pyocd.core.target import Target  # noqa: E402
from pyocd.flash.file_programmer import FileProgrammer  # noqa: E402

FLASH_BASE = 0x08000000
LOOP_PC = FLASH_BASE + 0x40
DAP_IDCODE = 0x3ba00477
IMAGE_SIZE = 32 * 1024


def make_image():
    vectors = struct.pack("<16I", 0x20005000, *([LOOP_PC | 1] * 15))
    code = struct.pack("<2H", 0x3101, 0xe7fd)  # adds r1, #1 ; b .-2 (back to the adds)
    head = vectors.ljust(0x40, b"\xff") + code
    rnd = random.Random(0x5A17)
    return head + bytes(rnd.getrandbits(8) for _ in range(IMAGE_SIZE - len(head)))


ok = True


def check(name, cond, extra=""):
    global ok
    ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + name, extra)


def check_dap_commands(probe):
    """Raw DAP_JTAG_Configure and DAP_JTAG_IDCODE commands."""
    proto = probe._link._protocol
    probe.jtag_sequence(6, 1, False, 0x3f)  # Test-Logic-Reset
    probe.jtag_sequence(1, 0, False, 0x1)   # Run-Test/Idle
    proto.jtag_configure(CHAIN_IR_LENGTHS)
    check("IDCODE device 0", proto.jtag_id_code(0) == DAP_IDCODE, hex(proto.jtag_id_code(0)))

    def fails(fn):
        try:
            fn()
        except Exception:
            return True
        return False

    check("IDCODE device 2 (not in chain) replies an error", fails(lambda: proto.jtag_id_code(2)))
    for bad in ([4] * 9, [0, 4], [33]):
        check("DAP_JTAG_Configure %s replies an error" % bad, fails(lambda: _orig_jtag_configure(proto, bad)))
    proto.jtag_configure(CHAIN_IR_LENGTHS)  # chain is empty after an error


def main():
    global ok
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("freq", type=int, nargs="?", default=1000000, help="TCK or SWCLK in Hz (default 1000000)")
    parser.add_argument("--protocol", choices=("jtag", "swd"), default="jtag")
    parser.add_argument("--target", default="stm32f103rc", help="pyOCD target type (default stm32f103rc)")
    parser.add_argument("--uid", help="the unique ID (USB serial number) of the probe")
    args = parser.parse_args()
    freq = args.freq
    jtag = args.protocol == "jtag"
    if jtag:
        core.CMSISDAPProtocol.jtag_configure = lambda self, devices_irlen=None: _orig_jtag_configure(
            self, CHAIN_IR_LENGTHS)
    data = make_image()
    session = ConnectHelper.session_with_chosen_probe(
        blocking=False, unique_id=args.uid, target_override=args.target,
        options={"dap_protocol": args.protocol, "frequency": 1000000, "connect_mode": "under-reset"})
    if session is None:
        print("RESULT FAIL: no probe found")
        return 1
    with session:
        t = session.target
        probe = session.probe

        def fast_clock():
            if freq <= 1000000:
                return
            w, r = t.write32, t.read32
            w(0x40022000, 0x12)                 # FLASH_ACR: 2 wait states, prefetch
            w(0x40021004, 0x00380000)           # RCC_CFGR: PLLMUL16, PLLSRC = HSI / 2
            w(0x40021000, r(0x40021000) | (1 << 24))
            t0 = time.time()
            while not (r(0x40021000) & (1 << 25)) and time.time() - t0 < 1:
                pass
            check("PLL locked", r(0x40021000) & (1 << 25))
            w(0x40021004, (r(0x40021004) & ~3) | 2)
            check("SYSCLK = PLL", ((r(0x40021004) >> 2) & 3) == 2)
            probe.set_clock(freq)

        def slow_clock():
            probe.set_clock(1000000)

        if jtag:
            check_dap_commands(probe)
        t.halt()
        check("halted", t.get_state() == Target.State.HALTED)
        fast_clock()
        t0 = time.time()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "image.bin")
            open(path, "wb").write(data)
            FileProgrammer(session).program(path, base_address=FLASH_BASE, file_format="bin")
        print("program time %.2fs" % (time.time() - t0))
        t0 = time.time()
        rb = bytes(t.read_memory_block8(FLASH_BASE, len(data)))
        print("read time %.2fs" % (time.time() - t0))
        check("flash read-back", rb == data, "%d bytes" % len(data))

        slow_clock()
        t.reset_and_halt()
        check("reset_and_halt pc", t.read_core_register("pc") == LOOP_PC, hex(t.read_core_register("pc")))
        check("sp from vector table", t.read_core_register("sp") == 0x20005000, hex(t.read_core_register("sp")))
        fast_clock()
        r1 = t.read_core_register("r1")
        t.step()
        t.step()
        check("two steps run one adds", t.read_core_register("r1") == r1 + 1,
              "r1 %d -> %d" % (r1, t.read_core_register("r1")))

        # Hardware breakpoint on the adds: the loop must stop there.
        t.set_breakpoint(LOOP_PC)
        t.resume()
        t0 = time.time()
        while t.get_state() != Target.State.HALTED and time.time() - t0 < 1:
            time.sleep(0.01)
        check("hardware breakpoint halts", t.get_state() == Target.State.HALTED
              and t.read_core_register("pc") == LOOP_PC, hex(t.read_core_register("pc")))
        t.remove_breakpoint(LOOP_PC)

        vals = [0xdeadbeef, 0x12345678, 0xa5a5a5a5, 0x5a5a5a5a]
        t.write_memory_block32(0x20000000, vals)
        check("RAM write/read", t.read_memory_block32(0x20000000, 4) == vals)
        big = bytes((i * 7 + 3) & 0xff for i in range(8192))
        t.write_memory_block8(0x20000100, big)
        check("RAM block 8 KiB", bytes(t.read_memory_block8(0x20000100, 8192)) == big)

        t.resume()
        time.sleep(0.3)
        check("running", t.get_state() == Target.State.RUNNING)
        t.halt()
        check("halt after run", t.get_state() == Target.State.HALTED)
        slow_clock()
        t.reset()
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

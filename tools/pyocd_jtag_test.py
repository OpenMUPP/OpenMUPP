#!/usr/bin/env python3
"""pyOCD JTAG test of an STM32F1 target on the CMSIS-DAP personality.

pyOCD 0.45.1 sends DAP_JTAG_Configure with one device of IR length 4. The
STM32F1 has two TAPs: the Cortex-M3 DAP (IR length 4, next to TDO) and the
boundary-scan TAP (IR length 5). Thus the script patches the pyOCD protocol
class to configure the chain with the IR lengths 4 and 5.

The script flashes an image, reads it back, resets and halts the target,
and steps, runs and halts it for a loop image (see below). Above 1000 kHz it sets the core clock of the
target to 64 MHz (HSI / 2 x 16) first, because TCK must not be above
F_CPU / 6. The target clock goes back to 8 MHz at each reset.

The image with "loop" in its file name must have a vector table with
SP = 0x20005000 and reset vector 0x08000041, and the code `adds r1, #1;
b .-4` at 0x08000040. Other images only get the flash and RAM checks.

The script erases the flash of the target.

Usage:
    python3 pyocd_jtag_test.py 4500000 image.bin
"""
import sys, time
from pyocd.probe.pydapaccess import cmsis_dap_core as core
_orig = core.CMSISDAPProtocol.jtag_configure
def patched(self, devices_irlen=None):
    return _orig(self, [4, 5])          # device 0 = CPU DAP (next to TDO), device 1 = boundary scan
core.CMSISDAPProtocol.jtag_configure = patched

from pyocd.core.helpers import ConnectHelper
from pyocd.flash.file_programmer import FileProgrammer
from pyocd.core.target import Target

freq = int(sys.argv[1]); img = sys.argv[2]
is_loop = "loop" in img
ok = True
def check(name, cond, extra=""):
    global ok
    ok &= bool(cond); print(("PASS " if cond else "FAIL ") + name, extra)

with ConnectHelper.session_with_chosen_probe(target_override="stm32f103rc",
        options={"dap_protocol": "jtag", "frequency": 1000000, "connect_mode": "under-reset"}) as session:
    t = session.target
    probe = session.probe
    def fast_clock():
        """Core clock 64 MHz (HSI/2 x16) as in tools/stm32f1_clock.tcl, then TCK = freq."""
        if freq <= 1000000: return
        w, r = t.write32, t.read32
        w(0x40022000, 0x12)
        w(0x40021004, 0x00380000)
        w(0x40021000, r(0x40021000) | (1 << 24))
        t0 = time.time()
        while not (r(0x40021000) & (1 << 25)) and time.time() - t0 < 1: pass
        check("PLL locked", r(0x40021000) & (1 << 25))
        w(0x40021004, (r(0x40021004) & ~3) | 2)
        check("SYSCLK = PLL", ((r(0x40021004) >> 2) & 3) == 2)
        probe.set_clock(freq)
    def slow_clock():
        probe.set_clock(1000000)

    t.halt()
    check("halted", t.get_state() == Target.State.HALTED)
    fast_clock()
    t0 = time.time()
    FileProgrammer(session).program(img, base_address=0x08000000, file_format="bin")
    print("program time %.2fs" % (time.time() - t0))
    data = open(img, "rb").read()
    t0 = time.time()
    rb = bytes(t.read_memory_block8(0x08000000, len(data)))
    print("read time %.2fs" % (time.time() - t0))
    check("flash read-back", rb == data, "%d bytes" % len(data))
    slow_clock()
    t.reset_and_halt()
    if is_loop:
        check("reset_and_halt pc == 0x08000040", t.read_core_register("pc") == 0x08000040, hex(t.read_core_register("pc")))
        check("sp from vector table", t.read_core_register("sp") == 0x20005000, hex(t.read_core_register("sp")))
        r1 = t.read_core_register("r1")
        t.step(); t.step()
        check("step runs code", t.read_core_register("r1") == r1 + 1, "r1 %d -> %d pc %#x" % (r1, t.read_core_register("r1"), t.read_core_register("pc")))
    fast_clock()
    vals = [0xdeadbeef, 0x12345678, 0xa5a5a5a5, 0x5a5a5a5a]
    t.write_memory_block32(0x20000000, vals)
    check("RAM write/read", t.read_memory_block32(0x20000000, 4) == vals)
    big = bytes((i * 7 + 3) & 0xff for i in range(8192))
    t.write_memory_block8(0x20000100, big)
    check("RAM block 8 KiB", bytes(t.read_memory_block8(0x20000100, 8192)) == big)
    if is_loop:
        t.resume(); time.sleep(0.3)
        check("running", t.get_state() == Target.State.RUNNING)
        t.halt()
        check("halt after run", t.get_state() == Target.State.HALTED)
    slow_clock()
    t.reset()
print("RESULT", "PASS" if ok else "FAIL")

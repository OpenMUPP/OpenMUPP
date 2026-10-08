# SPDX-License-Identifier: GPL-2.0-or-later
#
# OpenOCD procedure that sets the core clock of a halted STM32F1 target to
# 64 MHz: HSI / 2 x 16 with the PLL. After reset the core runs at 8 MHz
# (HSI), and the JTAG clock must not be faster than F_CPU / 6. 64 MHz allows
# 10.6 MHz TCK.
#
# The procedure does not use the HSE: the crystal frequency of the target
# board is not known, and a wrong PLL factor overclocks the core. 72 MHz
# needs the HSE; add it only for a board with a known crystal.
#
# The HSI stays on: the flash programming interface needs it.
# A reset sets the clock back to 8 MHz. Thus call the procedure after each
# reset, and set the adapter speed after the procedure.
#
# Usage: openocd ... -f tools/stm32f1_clock.tcl -c init -c "reset halt" \
#                    -c stm32f1_clock_max -c "adapter speed 9000"

set _STM32F1_RCC_CR    0x40021000
set _STM32F1_RCC_CFGR  0x40021004
set _STM32F1_FLASH_ACR 0x40022000

# Returns 1 when (value at addr & mask) == value within about 100 ms.
proc _stm32f1_wait_bits {addr mask value} {
	for {set i 0} {$i < 100} {incr i} {
		if {([mrw $addr] & $mask) == $value} {
			return 1
		}
		sleep 1
	}
	return 0
}

proc stm32f1_clock_max {} {
	global _STM32F1_RCC_CR _STM32F1_RCC_CFGR _STM32F1_FLASH_ACR

	# Start from the reset state: SYSCLK = HSI, PLL off.
	mmw $_STM32F1_RCC_CFGR 0 0x3
	if {![_stm32f1_wait_bits $_STM32F1_RCC_CFGR 0xC 0x0]} {
		error "stm32f1_clock_max: SYSCLK does not change to the HSI"
	}
	# The PLL factors must not change while the PLL is on.
	mmw $_STM32F1_RCC_CR 0 0x01000000
	if {![_stm32f1_wait_bits $_STM32F1_RCC_CR 0x02000000 0x0]} {
		error "stm32f1_clock_max: the PLL does not stop"
	}

	# Two flash wait states for 48 to 72 MHz, prefetch buffer on.
	mww $_STM32F1_FLASH_ACR 0x32

	# PLLSRC = HSI / 2, PLLMUL = 16, APB1 = HCLK / 2 (max 36 MHz).
	mww $_STM32F1_RCC_CFGR 0x003C0400

	# PLLON, wait for PLLRDY, then SW = PLL and wait for SWS = PLL.
	mmw $_STM32F1_RCC_CR 0x01000000 0
	if {![_stm32f1_wait_bits $_STM32F1_RCC_CR 0x02000000 0x02000000]} {
		error "stm32f1_clock_max: the PLL does not lock"
	}
	mmw $_STM32F1_RCC_CFGR 0x2 0x3
	if {![_stm32f1_wait_bits $_STM32F1_RCC_CFGR 0xC 0x8]} {
		error "stm32f1_clock_max: SYSCLK does not change to the PLL"
	}
	echo "target clock: 64 MHz (HSI)"
}

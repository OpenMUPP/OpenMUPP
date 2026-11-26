// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The scale of the Vref sense: ADC reading to the target voltage in mV.
// No CMSIS dependency, so the host unit tests check it.

#include <cstdint>

namespace board {

// The ADC reference (VDDA) in mV. The boards supply the ADC with 3.3 V from
// the regulator, as the original firmware assumes (TVCC_SAMPLE_VREF). The
// error of the regulator is in the Vref result.
inline constexpr uint32_t vddaMillivolts = 3300;

// Highest value of a 12-bit conversion
inline constexpr uint32_t adcMaxRaw = 4095;

// The voltage in mV at the input of the divider for an ADC reading, rounded
// to the nearest mV. The result stops at 65535 mV.
constexpr uint16_t vrefMillivolts(uint32_t raw, uint8_t divider) {
   const uint32_t mv = (raw * vddaMillivolts * divider + adcMaxRaw / 2) / adcMaxRaw;
   return static_cast<uint16_t>(mv > 0xFFFF ? 0xFFFF : mv);
}

static_assert(vrefMillivolts(0, 2) == 0);
static_assert(vrefMillivolts(adcMaxRaw, 1) == 3300);
static_assert(vrefMillivolts(adcMaxRaw, 2) == 6600);
// 3.3 V on the target is 1.65 V at the pin: half of the full scale
static_assert(vrefMillivolts(2048, 2) == 3301);
static_assert(vrefMillivolts(adcMaxRaw, 255) == 0xFFFF);

// The VPP senses of OpenMUPP v1: R47 (100 kOhm) and R48 (27 kOhm), the same
// ratio for R50 and R51. The voltage in mV at the divider input, rounded.
constexpr uint16_t vppSenseMillivolts(uint32_t raw) {
   constexpr uint32_t top = 100;
   constexpr uint32_t bottom = 27;
   constexpr uint32_t den = adcMaxRaw * bottom;
   return static_cast<uint16_t>((raw * vddaMillivolts * (top + bottom) + den / 2) / den);
}

static_assert(vppSenseMillivolts(0) == 0);
// The full scale is about 15.5 V (docs/hardware.md of openmupp-v1)
static_assert(vppSenseMillivolts(adcMaxRaw) == 15522);
// 1.06 V at the pin is about 5 V
static_assert(vppSenseMillivolts(1315) == 4985);

} // namespace board

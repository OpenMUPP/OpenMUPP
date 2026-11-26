// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardVpp.h"

#if defined(CH32V30x_D8C)
#include <cmsis_device.h>
#endif

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/VrefScale.h"

namespace board {

namespace {

static_assert(!current.vpp || current.family == Family::Ch32V30x, "BoardVpp has the DAC code of the CH32V307 only");

#if defined(CH32V30x_D8C)
// The DAC of the CH32V307 (CH32FV2x_V3xRM chapter 17): the header of
// ch32v00x-system has no definitions for it.
struct DacRegisters {
   volatile uint32_t CTLR;
   volatile uint32_t SWTR;
   volatile uint32_t R12BDHR1;
   volatile uint32_t L12BDHR1;
   volatile uint32_t R8BDHR1;
   volatile uint32_t R12BDHR2;
};
constexpr uintptr_t dacBase = 0x40007400;
constexpr uint32_t rccDacEn = 1ul << 29;
// Channel 2 (PA5) on, with the output buffer (0 V to 3.29 V, CH32V307DS0
// table 4-46), no trigger: a write goes to the output in one PCLK1 cycle.
constexpr uint32_t dacEn2 = 1ul << 16;

DacRegisters &dac() {
   return *reinterpret_cast<DacRegisters *>(dacBase);
}
#endif

} // namespace

BoardVpp::BoardVpp(const BoardPower &power)
   : power_(power) {
}

void BoardVpp::setDac([[maybe_unused]] uint16_t code) {
#if defined(CH32V30x_D8C)
   if(!current.vpp) {
      return;
   }
   gpioPin(current.vpp->dac).configure(hw::GpioPin::Mode::Analog);
   RCC->APB1PCENR |= rccDacEn;
   // The data first, so the channel starts at the new level
   dac().R12BDHR2 = code & 0xFFFu;
   dac().CTLR |= dacEn2;
#endif
}

void BoardVpp::dacOff() {
#if defined(CH32V30x_D8C)
   if(!current.vpp) {
      return;
   }
   // The pin stays analog: high-impedance, VPP 11.4 V with VPP_EN high
   dac().CTLR &= ~dacEn2;
   RCC->APB1PCENR &= ~rccDacEn;
#endif
}

void BoardVpp::setEnable(bool on) {
   if(current.vpp) {
      gpioPin(current.vpp->enable).write(on);
   }
}

void BoardVpp::setOn(bool on) {
   if(current.vpp) {
      gpioPin(current.vpp->on).write(on);
   }
}

void BoardVpp::setPullDown(bool on) {
   if(current.vpp) {
      gpioPin(current.vpp->pullDown).write(on);
   }
}

std::optional<uint16_t> BoardVpp::senseMillivolts() {
   if(!current.vpp) {
      return std::nullopt;
   }
   return measure(current.vpp->sense);
}

std::optional<uint16_t> BoardVpp::sense2Millivolts() {
   if(!current.vpp) {
      return std::nullopt;
   }
   return measure(current.vpp->sense2);
}

std::optional<uint16_t> BoardVpp::measure(Pin pin) const {
   const auto raw = power_.meanRaw(pin);
   if(!raw) {
      return std::nullopt;
   }
   return vppSenseMillivolts(*raw);
}

} // namespace board

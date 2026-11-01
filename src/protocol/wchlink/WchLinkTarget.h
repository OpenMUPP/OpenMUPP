// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The target side of the WCH-Link processor: the RISC-V debug module of a
// CH32V target on the wire engine. The processor (WchLinkProcessor.h) uses
// this interface only, so it does not depend on the pins.
// RvSwdTarget<Pins> implements it with rvswd::RvSwdEngine and
// rvswd::DebugModule. The host unit tests use the same adapter on mock pins
// with a mock DM (tests/MockDebugModule.h).

#include <cstdint>

#include "protocol/Background.h"
#include "rvswd/DebugModule.h"
#include "rvswd/RvSwdEngine.h"

namespace wchlink {

// The WCH DM registers that the connect writes with unlockValue, also in
// the RISC-V bridge of CMSIS-DAP (cmsisdap::RvDtmTap)
namespace wchdm {
inline constexpr uint8_t unlock1 = 0x7e;
inline constexpr uint8_t unlock2 = 0x7d;
inline constexpr uint32_t unlockValue = 0x5aa5'0400;
} // namespace wchdm

// The speed byte of the configure command (0x0c)
enum class Speed : uint8_t {
   High = 1,
   Medium = 2,
   Low = 3,
};

class Target {
   public:
   // Drives the pins for the wire and sends a line reset.
   virtual void attach(rvswd::Wire wire) = 0;
   // Releases the pins.
   virtual void detach() = 0;
   virtual void setSpeed(Speed speed) = 0;

   // As rvswd::DebugModule
   virtual rvswd::Status dmiRead(uint8_t address, uint32_t &data) = 0;
   virtual rvswd::Status dmiWrite(uint8_t address, uint32_t data) = 0;
   virtual rvswd::Status readDmstatus(uint32_t &value) = 0;
   virtual rvswd::Status halted(bool &halted) = 0;
   virtual rvswd::Status halt() = 0;
   virtual rvswd::Status resume() = 0;
   virtual rvswd::Status readRegister(uint16_t regno, uint32_t &value) = 0;
   virtual rvswd::Status writeRegister(uint16_t regno, uint32_t value) = 0;
   virtual rvswd::Status readMemory(uint32_t address, uint32_t &value) = 0;
   virtual rvswd::Status writeMemory(uint32_t address, uint32_t value, rvswd::Width width) = 0;
   // The method after attach() is the method of the wire. A target can need the other one.
   virtual void setMemoryAccess(rvswd::MemoryAccess access) = 0;

   protected:
   ~Target() = default;
};

// Two wires: the high speed uses the SWCLK rate of the writes of the
// WCH-LinkE at its default speed. The medium and the low speed use the rate
// of its reads. One wire: the speed has no effect, as on the WCH-LinkE
// (docs/wch-link-protocol.md, "Speed setting").
constexpr uint32_t clockHzOf(Speed speed) {
   return speed == Speed::High ? rvswd::highClockHz : rvswd::lowClockHz;
}

template <typename Pins>
class RvSwdTarget final : public Target {
   public:
   // background: serviced by the engine, nullptr for none.
   explicit RvSwdTarget(Pins &pins, protocol::Background *background = nullptr)
      : engine_(pins, background)
      , module_(engine_) {
   }

   void attach(rvswd::Wire wire) override {
      engine_.init(wire);
      engine_.setClockHz(clockHz_);
      engine_.lineReset();
      module_.init();
   }

   void detach() override {
      engine_.fini();
   }

   void setSpeed(Speed speed) override {
      clockHz_ = clockHzOf(speed);
      engine_.setClockHz(clockHz_);
   }

   rvswd::Status dmiRead(uint8_t address, uint32_t &data) override {
      return module_.dmiRead(address, data);
   }
   rvswd::Status dmiWrite(uint8_t address, uint32_t data) override {
      return module_.dmiWrite(address, data);
   }
   rvswd::Status readDmstatus(uint32_t &value) override {
      return module_.readDmstatus(value);
   }
   rvswd::Status halted(bool &halted) override {
      return module_.halted(halted);
   }
   rvswd::Status halt() override {
      return module_.halt();
   }
   rvswd::Status resume() override {
      return module_.resume();
   }
   rvswd::Status readRegister(uint16_t regno, uint32_t &value) override {
      return module_.readRegister(regno, value);
   }
   rvswd::Status writeRegister(uint16_t regno, uint32_t value) override {
      return module_.writeRegister(regno, value);
   }
   rvswd::Status readMemory(uint32_t address, uint32_t &value) override {
      return module_.readMemory(address, value);
   }
   rvswd::Status writeMemory(uint32_t address, uint32_t value, rvswd::Width width) override {
      return module_.writeMemory(address, value, width);
   }
   void setMemoryAccess(rvswd::MemoryAccess access) override {
      module_.setMemoryAccess(access);
   }

   private:
   rvswd::RvSwdEngine<Pins> engine_;
   rvswd::DebugModule<rvswd::RvSwdEngine<Pins>> module_;
   uint32_t clockHz_{ clockHzOf(Speed::High) };
};

} // namespace wchlink

#pragma once

// A JTAG chain for the mock JTAG pins (MockJtagPins.h): the TAP state
// machine, and devices with an IR, IDCODE and BYPASS. A device can be an
// Arm JTAG-DP (ADIv5) with a memory AP. Device 0 is next to TDO.
//
// The JTAG-DP model:
//
// - DPACC and APACC scans have 35 bits: RnW, A[3:2], then 32 data bits.
//   Capture-DR loads the acknowledge (OK/FAULT 0b010, WAIT 0b001) and the
//   result of the last read. Update-DR runs the request.
// - After each AP access, the next apWaits captures give WAIT, and their
//   requests are dropped.
// - DP registers: DPIDR (0x0), CTRL/STAT (0x4), SELECT (0x8), RDBUFF
//   (0xC). CTRL/STAT gives the power-up acknowledge bits of the request
//   bits. A read of RDBUFF gives 0.
// - The AP (bank of SELECT): CSW (0x00), TAR (0x04), DRW (0x0C) on a word
//   memory, TAR increments by 4 after each DRW access, IDR (0xFC).
// - ABORT scans are recorded.
// - Test-Logic-Reset loads the IDCODE instruction of each device into its
//   IR. A device has its own IDCODE instruction (0x0E for an Arm
//   JTAG-DP).

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "MockJtagPins.h"

namespace mupptest {

namespace tapIr {
inline constexpr uint32_t abort = 0x08;
inline constexpr uint32_t dpacc = 0x0A;
inline constexpr uint32_t apacc = 0x0B;
inline constexpr uint32_t idcode = 0x0E;
} // namespace tapIr

class MockJtagChain final : public JtagTarget {
   public:
   enum class State : uint8_t {
      Reset,
      Idle,
      SelectDr,
      CaptureDr,
      ShiftDr,
      Exit1Dr,
      PauseDr,
      Exit2Dr,
      UpdateDr,
      SelectIr,
      CaptureIr,
      ShiftIr,
      Exit1Ir,
      PauseIr,
      Exit2Ir,
      UpdateIr,
   };

   struct Device {
      uint8_t irLength;
      uint32_t idcode;
      bool dp;
      uint32_t idcodeIr{ tapIr::idcode };
   };

   // A JTAG-DP access at Update-DR
   struct Access {
      bool ap;
      bool read;
      uint8_t address;
      uint32_t data;

      bool operator==(const Access &) const = default;
   };

   explicit MockJtagChain(std::vector<Device> devices)
      : devices_(std::move(devices))
      , taps_(devices_.size()) {
      for(std::size_t i = 0; i < taps_.size(); ++i) {
         taps_[i].ir = devices_[i].idcodeIr;
      }
   }

   void rising(bool tms, bool tdi) override {
      switch(state) {
      case State::Idle:
         ++idleClocks;
         break;
      case State::CaptureIr:
         for(std::size_t i = 0; i < taps_.size(); ++i) {
            // The two low bits of a captured IR are 0b01.
            taps_[i].shift = 0x01;
            taps_[i].length = devices_[i].irLength;
         }
         break;
      case State::CaptureDr:
         for(std::size_t i = 0; i < taps_.size(); ++i) {
            captureDr(i);
         }
         break;
      case State::ShiftIr:
      case State::ShiftDr:
         shiftChain(tdi);
         break;
      default:
         break;
      }
      state = next(state, tms);
      if(state == State::Reset) {
         for(std::size_t i = 0; i < taps_.size(); ++i) {
            taps_[i].ir = devices_[i].idcodeIr;
         }
      }
   }

   bool falling() override {
      if(state == State::UpdateIr) {
         ++irScans;
         for(Tap &tap : taps_) {
            tap.ir = static_cast<uint32_t>(tap.shift);
         }
      } else if(state == State::UpdateDr) {
         ++drScans;
         for(std::size_t i = 0; i < taps_.size(); ++i) {
            updateDr(i);
         }
      }
      if(state == State::ShiftIr || state == State::ShiftDr) {
         tdo_ = (taps_.front().shift & 1u) != 0;
      }
      return tdo_ || tdoStuckHigh;
   }

   [[nodiscard]] uint32_t ir(std::size_t device) const {
      return taps_[device].ir;
   }

   // The TAP state
   State state{ State::Reset };
   // JTAG-DP
   uint32_t dpidr{ 0x2BA01477 };
   uint32_t ctrlStat{ 0 };
   uint32_t select{ 0 };
   uint32_t csw{ 0x03000042 };
   uint32_t tar{ 0 };
   uint32_t apIdr{ 0x24770011 };
   std::map<uint32_t, uint32_t> memory;
   uint32_t apWaits{ 0 };
   // TDO reads 1 all the time, as with no target on the connector
   bool tdoStuckHigh{ false };
   std::vector<Access> accesses;
   std::vector<uint32_t> aborts;
   // The WAIT acknowledges that the DP gave
   uint32_t waits{ 0 };
   // Rising edges of TCK in Run-Test/Idle, and the scans
   uint32_t idleClocks{ 0 };
   uint32_t irScans{ 0 };
   uint32_t drScans{ 0 };

   private:
   struct Tap {
      uint32_t ir{ 0 };
      uint64_t shift{ 0 };
      uint32_t length{ 1 };
      // The captured acknowledge was WAIT: Update-DR drops the request.
      bool waited{ false };
   };

   static State next(State state, bool tms) {
      switch(state) {
      case State::Reset:
         return tms ? State::Reset : State::Idle;
      case State::Idle:
         return tms ? State::SelectDr : State::Idle;
      case State::SelectDr:
         return tms ? State::SelectIr : State::CaptureDr;
      case State::CaptureDr:
      case State::ShiftDr:
         return tms ? State::Exit1Dr : State::ShiftDr;
      case State::Exit1Dr:
         return tms ? State::UpdateDr : State::PauseDr;
      case State::PauseDr:
         return tms ? State::Exit2Dr : State::PauseDr;
      case State::Exit2Dr:
         return tms ? State::UpdateDr : State::ShiftDr;
      case State::UpdateDr:
      case State::UpdateIr:
         return tms ? State::SelectDr : State::Idle;
      case State::SelectIr:
         return tms ? State::Reset : State::CaptureIr;
      case State::CaptureIr:
      case State::ShiftIr:
         return tms ? State::Exit1Ir : State::ShiftIr;
      case State::Exit1Ir:
         return tms ? State::UpdateIr : State::PauseIr;
      case State::PauseIr:
         return tms ? State::Exit2Ir : State::PauseIr;
      case State::Exit2Ir:
         return tms ? State::UpdateIr : State::ShiftIr;
      }
      return State::Reset;
   }

   [[nodiscard]] bool isAccess(std::size_t device) const {
      const uint32_t ir = taps_[device].ir;
      return devices_[device].dp && (ir == tapIr::dpacc || ir == tapIr::apacc || ir == tapIr::abort);
   }

   void captureDr(std::size_t device) {
      Tap &tap = taps_[device];
      tap.waited = false;
      if(isAccess(device)) {
         tap.length = 35;
         uint64_t ack = 0b010;
         if(tap.ir != tapIr::abort && pendingWaits_ != 0) {
            --pendingWaits_;
            ++waits;
            tap.waited = true;
            ack = 0b001;
         }
         tap.shift = static_cast<uint64_t>(result_) << 3 | ack;
      } else if(tap.ir == devices_[device].idcodeIr) {
         tap.length = 32;
         tap.shift = devices_[device].idcode;
      } else {
         // BYPASS captures 0.
         tap.length = 1;
         tap.shift = 0;
      }
   }

   // TDI goes into the last device, and device 0 drives TDO.
   void shiftChain(bool tdi) {
      bool in = tdi;
      for(std::size_t i = taps_.size(); i-- > 0;) {
         Tap &tap = taps_[i];
         const bool out = (tap.shift & 1u) != 0;
         tap.shift = (tap.shift >> 1) | (static_cast<uint64_t>(in) << (tap.length - 1));
         in = out;
      }
   }

   void updateDr(std::size_t device) {
      const Tap &tap = taps_[device];
      if(!isAccess(device) || tap.waited) {
         return;
      }
      const bool read = (tap.shift & 1u) != 0;
      const auto address = static_cast<uint8_t>(((tap.shift >> 1) & 0x03u) << 2);
      const auto data = static_cast<uint32_t>(tap.shift >> 3);
      if(tap.ir == tapIr::abort) {
         aborts.push_back(data);
         return;
      }
      const bool ap = tap.ir == tapIr::apacc;
      accesses.push_back({ ap, read, address, read ? 0 : data });
      if(ap) {
         apAccess(read, address, data);
         pendingWaits_ = apWaits;
      } else {
         dpAccess(read, address, data);
      }
   }

   void dpAccess(bool read, uint8_t address, uint32_t data) {
      if(!read) {
         if(address == 0x04) {
            ctrlStat = data;
         } else if(address == 0x08) {
            select = data;
         }
         return;
      }
      switch(address) {
      case 0x00:
         result_ = dpidr;
         break;
      case 0x04:
         // CDBGPWRUPACK and CSYSPWRUPACK follow their requests.
         result_ = ctrlStat | (ctrlStat & 0x50000000u) << 1;
         break;
      case 0x08:
         result_ = select;
         break;
      default:
         // RDBUFF
         result_ = 0;
         break;
      }
   }

   void apAccess(bool read, uint8_t address, uint32_t data) {
      const uint32_t reg = (select & 0xF0u) | address;
      uint32_t *target = nullptr;
      uint32_t idr = apIdr;
      switch(reg) {
      case 0x00:
         target = &csw;
         break;
      case 0x04:
         target = &tar;
         break;
      case 0x0C:
         target = &memory[tar];
         tar += 4;
         break;
      case 0xFC:
         target = &idr;
         break;
      default:
         break;
      }
      if(read) {
         result_ = target != nullptr ? *target : 0;
      } else if(target != nullptr && reg != 0xFC) {
         *target = data;
      }
   }

   std::vector<Device> devices_;
   std::vector<Tap> taps_;
   uint32_t result_{ 0 };
   uint32_t pendingWaits_{ 0 };
   bool tdo_{ false };
};

} // namespace mupptest

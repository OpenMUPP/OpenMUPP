// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardIdSense.h"

#include "board/Board.h"
#include "debug/DebugLog.h"

namespace board {

namespace {

constexpr const char *vendor = "OpenMUPP";

} // namespace

BoardTargetGate::BoardTargetGate(const BoardPower &power)
   : power_(power) {
}

void BoardTargetGate::init() {
   if constexpr(current.idSense.has_value()) {
      const auto raw = power_.meanRaw(current.idSense->boardId);
      board_ = raw ? idReadingOf(*raw) : IdReading{ IdKind::Fault, 0 };
      boardKnown_ = raw && isKnownBoardRevision(board_);
      if(!boardKnown_) {
         LOG_ERROR(debug::appLog, "board ID: not a known revision (slot %u): the probe does not drive the target",
                   board_.slot);
      }
      static_cast<void>(check(protocol::TargetProtocol::None));
   }
}

std::optional<IdReading> BoardTargetGate::readAdapterNow() const {
   if constexpr(current.idSense.has_value()) {
      const auto raw = power_.meanRaw(current.idSense->adapterId);
      if(raw) {
         return idReadingOf(*raw);
      }
   }
   return std::nullopt;
}

bool BoardTargetGate::readAdapter() {
   const auto reading = readAdapterNow();
   if(!reading) {
      return false;
   }
   adapter_ = *reading;
   formatAdapterVendor(vendorText_, vendor, adapter_);
   return true;
}

bool BoardTargetGate::check(protocol::TargetProtocol protocol) {
   const bool read = readAdapter();
   allowed_ = isTargetAllowed(boardKnown_, read ? std::optional<IdReading>(adapter_) : std::nullopt);
   if(!read) {
      LOG_ERROR(debug::appLog, "adapter ID: the ADC fails: the probe does not drive the target");
   } else if(!boardKnown_) {
      LOG_ERROR(debug::appLog, "board ID: not a known revision: the probe does not drive the target");
   } else if(adapter_.kind == IdKind::Fault) {
      LOG_ERROR(debug::appLog, "adapter ID: pin 16 is shorted (adapter fault): the probe does not drive the target");
   } else if(adapter_.kind == IdKind::None) {
      LOG_WARN(debug::appLog, "adapter ID: no adapter");
   } else if(adapter_.kind == IdKind::Unknown) {
      LOG_WARN(debug::appLog, "adapter ID: unknown adapter (slot %u)", adapter_.slot);
   } else if(!adapterMatches(protocol, adapter_)) {
      LOG_WARN(debug::appLog, "adapter ID: slot %u has no connector for %s", adapter_.slot, nameOf(protocol));
   }
   return allowed_;
}

} // namespace board

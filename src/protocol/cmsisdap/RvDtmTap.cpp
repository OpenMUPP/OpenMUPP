#include "protocol/cmsisdap/RvDtmTap.h"

#include <cstddef>

namespace cmsisdap {

namespace {

using State = RvDtmTap::State;

// The next state for TMS low and TMS high (IEEE 1149.1, figure 6-1)
struct Next {
   State low;
   State high;
};

constexpr Next next[] = {
   /* Reset */ { State::Idle, State::Reset },
   /* Idle */ { State::Idle, State::SelectDr },
   /* SelectDr */ { State::CaptureDr, State::SelectIr },
   /* CaptureDr */ { State::ShiftDr, State::Exit1Dr },
   /* ShiftDr */ { State::ShiftDr, State::Exit1Dr },
   /* Exit1Dr */ { State::PauseDr, State::UpdateDr },
   /* PauseDr */ { State::PauseDr, State::Exit2Dr },
   /* Exit2Dr */ { State::ShiftDr, State::UpdateDr },
   /* UpdateDr */ { State::Idle, State::SelectDr },
   /* SelectIr */ { State::CaptureIr, State::Reset },
   /* CaptureIr */ { State::ShiftIr, State::Exit1Ir },
   /* ShiftIr */ { State::ShiftIr, State::Exit1Ir },
   /* Exit1Ir */ { State::PauseIr, State::UpdateIr },
   /* PauseIr */ { State::PauseIr, State::Exit2Ir },
   /* Exit2Ir */ { State::ShiftIr, State::UpdateIr },
   /* UpdateIr */ { State::Idle, State::SelectDr },
};
static_assert(sizeof(next) / sizeof(next[0]) == static_cast<std::size_t>(State::UpdateIr) + 1);

bool bit(art::span<const uint8_t> bytes, uint32_t index) {
   return ((bytes[index / 8] >> (index % 8)) & 1u) != 0;
}

} // namespace

bool RvDtmTap::shift(uint32_t bitCount, art::span<const uint8_t> tdi, art::span<const uint8_t> tms, art::span<uint8_t> tdo) {
   for(uint32_t i = 0; i < bitCount; ++i) {
      if(clock(bit(tms, i), bit(tdi, i))) {
         tdo[i / 8] = static_cast<uint8_t>(tdo[i / 8] | (1u << (i % 8)));
      }
   }
   return true;
}

bool RvDtmTap::clock(bool tms, bool tdi) {
   bool out = true;
   if(state_ == State::ShiftDr || state_ == State::ShiftIr) {
      out = (shift_ & 1u) != 0;
      shift_ = (shift_ >> 1) | (static_cast<uint32_t>(tdi) << (length_ - 1));
   }
   const Next &n = next[static_cast<std::size_t>(state_)];
   state_ = tms ? n.high : n.low;
   switch(state_) {
   case State::Reset:
      ir_ = rvDtm::irIdcode;
      break;
   case State::CaptureIr:
      shift_ = rvDtm::irCapture;
      length_ = rvDtm::irLength;
      break;
   case State::UpdateIr:
      ir_ = static_cast<uint8_t>(shift_);
      break;
   case State::CaptureDr:
      captureDr();
      break;
   default:
      break;
   }
   return out;
}

void RvDtmTap::captureDr() {
   switch(ir_) {
   case rvDtm::irIdcode:
      shift_ = rvDtm::idcode;
      length_ = 32;
      break;
   case rvDtm::irDtmcs:
      shift_ = rvDtm::dtmcs;
      length_ = 32;
      break;
   default:
      shift_ = 0;
      length_ = 1;
      break;
   }
}

} // namespace cmsisdap

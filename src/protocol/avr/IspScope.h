// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The AVR ISP scope of the EDBG protocol: the STK500v2 programming commands
// (AVR068) as the EDBG tools take them ("EDBG-based Tools Protocols",
// DS50002630A, chapter 8). Hardware-independent: it runs on an
// avr::IspEngine (avr/IspEngine.h), and the host unit tests use a mock
// ISP target. The EDBG transport gives one command and sends the response.
//
// A command is [id][fields]. The response is [id][status][data]. NumBytes
// and the address are MSB first, the baud rate is LSB first. Commands:
//
//   0x06 LOAD_ADDRESS [address u32]. Flash: the word address, EEPROM: the
//        byte address. Bit 31 set: the flash is larger than 64 KB, and the
//        handler sends the Load Extended Address instruction (0x4D) before
//        an access that has a new address bits 16-23.
//   0x10 ENTER_PROGMODE [timeout][stabDelay][cmdexeDelay][synchLoops]
//        [byteDelay][pollValue][pollIndex][cmd 4]: drives the pins and
//        enables the programming mode (IspEngine::enableProgramming).
//        timeout is not used. Without the sync, the pins are released.
//   0x11 LEAVE_PROGMODE [preDelay][postDelay]: releases the pins.
//   0x12 CHIP_ERASE [eraseDelay][pollMethod][cmd 4]. pollMethod 0: wait
//        eraseDelay, 1: RDY/BSY polling.
//   0x13 PROGRAM_FLASH, 0x15 PROGRAM_EEPROM [NumBytes u16][mode][delay]
//        [cmd1][cmd2][cmd3][poll1][poll2][data]. See the mode byte below.
//   0x14 READ_FLASH, 0x16 READ_EEPROM [NumBytes u16][cmd1]
//        -> [id][OK][data][OK]
//   0x17 PROGRAM_FUSE, 0x19 PROGRAM_LOCK [cmd 4]
//   0x18 READ_FUSE, 0x1A READ_LOCK, 0x1B READ_SIGNATURE, 0x1C READ_OSCCAL
//        [retAddr][cmd 4] -> [id][OK][response byte retAddr (1-4)][OK]
//   0x1D SET_BAUD [kHz u16]: the SCK frequency (IspEngine::setSpeed).
//   0x1E GET_BAUD -> [id][kHz u16][OK]
//
// For flash, the handler sets the low/high byte bit (0x08 in cmd1 and
// cmd3) and the address of each byte. The first byte of a command is a low
// byte. Each byte of a read or program moves the address: one byte for
// EEPROM, one word for two bytes of flash.
//
// The mode byte of PROGRAM_FLASH and PROGRAM_EEPROM:
//
//   bit 0    0: word mode (cmd1 writes one byte), 1: page mode (cmd1 loads
//            the page buffer, cmd2 writes the page)
//   bits 1-3 word mode: timed delay, value polling, RDY/BSY polling
//   bits 4-6 page mode: timed delay, value polling, RDY/BSY polling
//   bit 7    page mode: write the page after the load
//
// Value polling reads the byte with cmd3 until it has the written value. A
// byte equal to poll1 (or to poll2 for EEPROM) cannot show the end, so the
// handler waits the delay. In page mode it polls the first byte of the
// command that it can poll. RDY/BSY polling sends 0xF0 0x00 0x00 0x00 until
// bit 0 of the last response byte is 0. A poll that does not end in
// pollTimeoutMs gives STATUS_CMD_TOUT (value) or STATUS_RDY_BSY_TOUT.
//
// Not supported: SPI_MULTI (the EDBG ISP scope has SET_BAUD on 0x1D), and
// the STK500v2 commands outside the scope (SIGN_ON, SET/GET_PARAMETER,
// OSCCAL). They get STATUS_CMD_UNKNOWN.
//
// All fields come from the host. The handler checks them before it moves a
// pin: a command that is too short, a NumBytes above maxDataBytes or above
// the data of the command, a retAddr out of 1-4, a pollIndex above 4 or a
// pollMethod above 1 gives STATUS_CMD_FAILED. Target commands before
// ENTER_PROGMODE also give STATUS_CMD_FAILED. A hardware fault of the
// engine gives STATUS_PHY_ERROR.

#include <array>
#include <cstddef>
#include <cstdint>

#include <utils/compat/span.h>

#include "avr/IspEngine.h"

namespace avr {

namespace isp {

namespace command {
inline constexpr uint8_t loadAddress = 0x06;
inline constexpr uint8_t enterProgmode = 0x10;
inline constexpr uint8_t leaveProgmode = 0x11;
inline constexpr uint8_t chipErase = 0x12;
inline constexpr uint8_t programFlash = 0x13;
inline constexpr uint8_t readFlash = 0x14;
inline constexpr uint8_t programEeprom = 0x15;
inline constexpr uint8_t readEeprom = 0x16;
inline constexpr uint8_t programFuse = 0x17;
inline constexpr uint8_t readFuse = 0x18;
inline constexpr uint8_t programLock = 0x19;
inline constexpr uint8_t readLock = 0x1A;
inline constexpr uint8_t readSignature = 0x1B;
inline constexpr uint8_t readOsccal = 0x1C;
inline constexpr uint8_t setBaud = 0x1D;
inline constexpr uint8_t getBaud = 0x1E;
} // namespace command

namespace status {
inline constexpr uint8_t ok = 0x00;
inline constexpr uint8_t cmdTimeout = 0x80;
inline constexpr uint8_t rdyBsyTimeout = 0x81;
inline constexpr uint8_t failed = 0xC0;
inline constexpr uint8_t unknown = 0xC9;
inline constexpr uint8_t phyError = 0xCB;
inline constexpr uint8_t baudInvalid = 0xCD;
} // namespace status

namespace mode {
inline constexpr uint8_t page = 0x01;
inline constexpr uint8_t wordDelay = 0x02;
inline constexpr uint8_t wordValue = 0x04;
inline constexpr uint8_t wordRdyBsy = 0x08;
inline constexpr uint8_t pageDelay = 0x10;
inline constexpr uint8_t pageValue = 0x20;
inline constexpr uint8_t pageRdyBsy = 0x40;
inline constexpr uint8_t writePage = 0x80;
} // namespace mode

// The data of one read or program command: the largest flash page of an
// AVR with ISP (256 bytes, ATmega2560), the block size of avrdude.
inline constexpr std::size_t maxDataBytes = 256;
// The response of a read of maxDataBytes: [id][OK][data][OK]
inline constexpr std::size_t maxResponseBytes = maxDataBytes + 3;
// The longest program or erase time of an AVR is some 10 ms.
inline constexpr uint32_t pollTimeoutMs = 100;

} // namespace isp

template <typename Engine>
class IspScope {
   public:
   explicit IspScope(Engine &engine)
      : engine_(engine) {
   }

   // Runs one command. Writes the response and returns its length.
   // response must hold isp::maxResponseBytes; with less, a read that does
   // not fit gives STATUS_CMD_FAILED. An empty request or a response
   // smaller than 4 bytes gives no response (0).
   std::size_t process(art::span<const uint8_t> request, art::span<uint8_t> response) {
      if(request.empty() || response.size() < 4) {
         return 0;
      }
      request_ = request;
      response_ = response;
      response_[0] = request[0];
      if(request[0] == isp::command::getBaud) {
         // [id][kHz u16][OK]: the status is the last byte.
         const uint32_t kHz = engine_.speedKHz();
         response_[1] = static_cast<uint8_t>(kHz);
         response_[2] = static_cast<uint8_t>(kHz >> 8);
         response_[3] = isp::status::ok;
         return 4;
      }
      length_ = 2;
      response_[1] = execute();
      return length_;
   }

   // True between a successful ENTER_PROGMODE and LEAVE_PROGMODE
   [[nodiscard]] bool programming() const {
      return programming_;
   }

   private:
   // Lengths of the commands, with the id
   static constexpr std::size_t addressBytes = 5;
   static constexpr std::size_t enterBytes = 12;
   static constexpr std::size_t leaveBytes = 3;
   static constexpr std::size_t eraseBytes = 7;
   static constexpr std::size_t programHeaderBytes = 10;
   static constexpr std::size_t readBytes = 4;
   static constexpr std::size_t fuseBytes = 5;
   static constexpr std::size_t readFuseBytes = 6;
   static constexpr std::size_t baudBytes = 3;

   static constexpr uint8_t highByte = 0x08;
   static constexpr uint8_t loadExtendedAddress = 0x4D;
   static constexpr uint32_t pollStepUs = 100;

   uint8_t execute() {
      namespace cmd = isp::command;
      const uint8_t id = request_[0];
      switch(id) {
      case cmd::loadAddress:
         return loadAddress();
      case cmd::enterProgmode:
         return enterProgmode();
      case cmd::leaveProgmode:
         return leaveProgmode();
      case cmd::setBaud:
         return setBaud();
      case cmd::chipErase:
      case cmd::programFlash:
      case cmd::readFlash:
      case cmd::programEeprom:
      case cmd::readEeprom:
      case cmd::programFuse:
      case cmd::programLock:
      case cmd::readFuse:
      case cmd::readLock:
      case cmd::readSignature:
      case cmd::readOsccal:
         if(!programming_) {
            return isp::status::failed;
         }
         return targetCommand(id);
      default:
         return isp::status::unknown;
      }
   }

   uint8_t targetCommand(uint8_t id) {
      namespace cmd = isp::command;
      switch(id) {
      case cmd::chipErase:
         return chipErase();
      case cmd::programFlash:
         return program(true);
      case cmd::programEeprom:
         return program(false);
      case cmd::readFlash:
         return read(true);
      case cmd::readEeprom:
         return read(false);
      case cmd::programFuse:
      case cmd::programLock:
         return programFuse();
      default:
         return readFuse();
      }
   }

   uint8_t loadAddress() {
      if(request_.size() < addressBytes) {
         return isp::status::failed;
      }
      const uint32_t value =
      (uint32_t{ request_[1] } << 24) | (uint32_t{ request_[2] } << 16) | (uint32_t{ request_[3] } << 8) | request_[4];
      extended_ = (value & 0x8000'0000u) != 0;
      address_ = value & 0x7FFF'FFFFu;
      extendedLoaded_ = false;
      return isp::status::ok;
   }

   uint8_t enterProgmode() {
      if(request_.size() < enterBytes || request_[7] > 4) {
         return isp::status::failed;
      }
      const EnableParams params{
         .stabDelayMs = request_[2],
         .cmdexeDelayMs = request_[3],
         .synchLoops = request_[4],
         .byteDelayMs = request_[5],
         .pollValue = request_[6],
         .pollIndex = request_[7],
         .command{ { request_[8], request_[9], request_[10], request_[11] } },
      };
      engine_.connect();
      // A new session: the target got a reset, its extended address is 0.
      extendedLoaded_ = false;
      programming_ = engine_.enableProgramming(params);
      if(!programming_) {
         engine_.disconnect();
         return isp::status::failed;
      }
      return isp::status::ok;
   }

   uint8_t leaveProgmode() {
      if(request_.size() < leaveBytes) {
         return isp::status::failed;
      }
      if(programming_) {
         engine_.waitMs(request_[1]);
         engine_.disconnect();
         programming_ = false;
         engine_.waitMs(request_[2]);
      }
      return isp::status::ok;
   }

   uint8_t setBaud() {
      if(request_.size() < baudBytes) {
         return isp::status::failed;
      }
      const uint32_t kHz = request_[1] | (uint32_t{ request_[2] } << 8);
      return engine_.setSpeed(kHz) ? isp::status::ok : isp::status::baudInvalid;
   }

   uint8_t chipErase() {
      if(request_.size() < eraseBytes || request_[2] > 1) {
         return isp::status::failed;
      }
      Instruction response{};
      if(!engine_.instruction({ { request_[3], request_[4], request_[5], request_[6] } }, response)) {
         return isp::status::phyError;
      }
      if(request_[2] == 0) {
         engine_.waitMs(request_[1]);
         return isp::status::ok;
      }
      const uint8_t result = waitReady();
      return result == isp::status::rdyBsyTimeout ? isp::status::cmdTimeout : result;
   }

   uint8_t programFuse() {
      if(request_.size() < fuseBytes) {
         return isp::status::failed;
      }
      Instruction response{};
      if(!engine_.instruction({ { request_[1], request_[2], request_[3], request_[4] } }, response)) {
         return isp::status::phyError;
      }
      return isp::status::ok;
   }

   uint8_t readFuse() {
      if(request_.size() < readFuseBytes || request_[1] < 1 || request_[1] > 4) {
         return isp::status::failed;
      }
      Instruction response{};
      if(!engine_.instruction({ { request_[2], request_[3], request_[4], request_[5] } }, response)) {
         return isp::status::phyError;
      }
      response_[2] = response[request_[1] - 1u];
      response_[3] = isp::status::ok;
      length_ = 4;
      return isp::status::ok;
   }

   uint8_t read(bool flash) {
      if(request_.size() < readBytes) {
         return isp::status::failed;
      }
      const std::size_t bytes = (std::size_t{ request_[1] } << 8) | request_[2];
      if(bytes > isp::maxDataBytes || response_.size() < bytes + 3) {
         return isp::status::failed;
      }
      for(std::size_t i = 0; i < bytes; ++i) {
         const bool high = flash && (i & 1u) != 0;
         if(!access(flash, request_[3], high, 0, &response_[2 + i])) {
            return isp::status::phyError;
         }
         advance(flash, high);
      }
      response_[2 + bytes] = isp::status::ok;
      length_ = bytes + 3;
      return isp::status::ok;
   }

   uint8_t program(bool flash) {
      if(request_.size() < programHeaderBytes) {
         return isp::status::failed;
      }
      const std::size_t bytes = (std::size_t{ request_[1] } << 8) | request_[2];
      if(bytes > isp::maxDataBytes || request_.size() < programHeaderBytes + bytes) {
         return isp::status::failed;
      }
      const uint8_t modeByte = request_[3];
      const uint8_t delayMs = request_[4];
      const uint8_t cmd1 = request_[5];
      const uint8_t cmd2 = request_[6];
      const uint8_t cmd3 = request_[7];
      const art::span<const uint8_t> data = request_.subspan(programHeaderBytes, bytes);
      const bool pageMode = (modeByte & isp::mode::page) != 0;
      // The mode bits of word mode, at the place of the page mode bits
      const uint8_t pollModes = pageMode ? static_cast<uint8_t>(modeByte >> 3) : modeByte;
      const uint32_t start = address_;
      // Page mode: the byte that value polling reads, -1 for none
      int pollIndex = -1;
      uint32_t pollAddress = 0;
      for(std::size_t i = 0; i < bytes; ++i) {
         const bool high = flash && (i & 1u) != 0;
         if(!access(flash, cmd1, high, data[i], nullptr)) {
            return isp::status::phyError;
         }
         if(!pageMode) {
            const uint8_t result = finishWrite(flash, pollModes, delayMs, cmd3, high, data[i]);
            if(result != isp::status::ok) {
               return result;
            }
         } else if(pollIndex < 0 && pollable(flash, data[i])) {
            pollIndex = static_cast<int>(i);
            pollAddress = address_;
         }
         advance(flash, high);
      }
      if(!pageMode || (modeByte & isp::mode::writePage) == 0) {
         return isp::status::ok;
      }
      // Write Program Memory Page at the address of the first byte: the
      // target takes the page address bits only.
      const uint32_t end = address_;
      address_ = start;
      if(!access(flash, cmd2, false, 0, nullptr)) {
         return isp::status::phyError;
      }
      // Without a byte to poll, finishWrite() gets a poll value: it uses
      // RDY/BSY polling or the delay.
      uint8_t result = isp::status::ok;
      if(pollIndex >= 0) {
         address_ = pollAddress;
         result = finishWrite(flash, pollModes, delayMs, cmd3, (pollIndex & 1) != 0 && flash, data[pollIndex]);
      } else {
         result = finishWrite(flash, pollModes, delayMs, cmd3, false, poll1());
      }
      address_ = end;
      return result;
   }

   // The end of one write, with the word mode bits of pollModes.
   uint8_t finishWrite(bool flash, uint8_t pollModes, uint8_t delayMs, uint8_t cmd3, bool high, uint8_t value) {
      if((pollModes & isp::mode::wordValue) != 0 && pollable(flash, value)) {
         return waitValue(flash, cmd3, high, value);
      }
      if((pollModes & isp::mode::wordRdyBsy) != 0) {
         return waitReady();
      }
      if((pollModes & (isp::mode::wordDelay | isp::mode::wordValue)) != 0) {
         engine_.waitMs(delayMs);
      }
      return isp::status::ok;
   }

   [[nodiscard]] uint8_t poll1() const {
      return request_[8];
   }

   // A byte that reads back as a poll value during the write cannot show
   // the end of the write.
   [[nodiscard]] bool pollable(bool flash, uint8_t value) const {
      return value != poll1() && (flash || value != request_[9]);
   }

   uint8_t waitValue(bool flash, uint8_t cmd3, bool high, uint8_t value) {
      for(uint32_t waited = 0; waited <= isp::pollTimeoutMs * 1000u; waited += pollStepUs) {
         uint8_t read = 0;
         if(!access(flash, cmd3, high, 0, &read)) {
            return isp::status::phyError;
         }
         if(read == value) {
            return isp::status::ok;
         }
         engine_.waitUs(pollStepUs);
      }
      return isp::status::cmdTimeout;
   }

   uint8_t waitReady() {
      for(uint32_t waited = 0; waited <= isp::pollTimeoutMs * 1000u; waited += pollStepUs) {
         Instruction response{};
         if(!engine_.instruction({ { 0xF0, 0x00, 0x00, 0x00 } }, response)) {
            return isp::status::phyError;
         }
         if((response[3] & 1u) == 0) {
            return isp::status::ok;
         }
         engine_.waitUs(pollStepUs);
      }
      return isp::status::rdyBsyTimeout;
   }

   // One instruction at the current address: [cmd][address bits 15-8]
   // [address bits 7-0][data]. read: gets the last response byte.
   bool access(bool flash, uint8_t cmd, bool high, uint8_t data, uint8_t *read) {
      if(flash && extended_) {
         const uint8_t extended = static_cast<uint8_t>(address_ >> 16);
         if(!extendedLoaded_ || extended != extendedByte_) {
            Instruction response{};
            if(!engine_.instruction({ { loadExtendedAddress, 0x00, extended, 0x00 } }, response)) {
               return false;
            }
            extendedByte_ = extended;
            extendedLoaded_ = true;
         }
      }
      const uint8_t first = high ? static_cast<uint8_t>(cmd | highByte) : cmd;
      const Instruction command{ { first, static_cast<uint8_t>(address_ >> 8), static_cast<uint8_t>(address_), data } };
      Instruction response{};
      if(!engine_.instruction(command, response)) {
         return false;
      }
      if(read != nullptr) {
         *read = response[3];
      }
      return true;
   }

   // Flash: the high byte moves the word address. EEPROM: each byte.
   void advance(bool flash, bool high) {
      if(!flash || high) {
         address_ = (address_ + 1u) & 0x7FFF'FFFFu;
      }
   }

   Engine &engine_;
   art::span<const uint8_t> request_;
   art::span<uint8_t> response_;
   std::size_t length_{ 0 };
   bool programming_{ false };
   uint32_t address_{ 0 };
   bool extended_{ false };
   bool extendedLoaded_{ false };
   uint8_t extendedByte_{ 0 };
};

} // namespace avr

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// AVR ISP engine: the serial programming interface of the AVR (SPI mode 0,
// MSB first, RESET low). It sends the 4-byte instructions of the AVR
// serial programming algorithm and enables the programming mode.
// Hardware-independent: a Pins type and an Spi type give the access, so the
// host unit tests run the engine on a mock ISP target. On target,
// board/BoardIsp.h gives them.
//
// The ISP signals use the JTAG pins: SCK on TCK, MOSI on TDI, MISO on TDO
// and RESET on SRST.
//
// The Pins type has these members:
//
//   void init();               SCK and MOSI outputs low, MISO input, RESET
//                              asserted (line low)
//   void fini();               SCK, MOSI inputs, RESET released
//   void begin();              before each operation: make now() count
//   void sck(bool), mosi(bool);
//   bool miso();
//   void reset(bool asserted); the board sets the pin level (polarity)
//   uint32_t now();            free-running core cycle counter
//   uint32_t coreHz();         core clock frequency
//
// The Spi type is default-constructible and has these members:
//
//   uint32_t clockHz();        SPI input clock: SCK = clockHz() >> shift
//   void init();               clocks and configuration with the shift of
//                              the last setShift(); the pins stay GPIO
//   void fini();               SPI off
//   void setShift(uint8_t);    shift spiMinShift-spiMaxShift
//   bool transfer(Pins &pins, const uint8_t *out, uint8_t *in,
//                 uint32_t bytes);
//                              mode 0, MSB first. SCK is low before and
//                              after, and the pins are GPIO before and
//                              after. False on a hardware timeout.
//
// Speed: the SPI gives clockHz() >> shift, shift 2-8. With SPI2 on the
// 72 MHz APB1 of the CH32V307, this is 18 MHz to 281 kHz. A request below
// the slowest SPI rate is bit-bang, at the requested rate: a new AVR runs
// at 1 MHz and needs SCK below 250 kHz, a 128 kHz target below 32 kHz.
// SCK is never faster than the request.
//
// Bit-bang bit, with SCK low at the start (mode 0):
//
//   1. Write MOSI. Wait for the low half period.
//   2. Read MISO. The target changed it at the last falling edge.
//   3. SCK high: the target samples MOSI. Wait for the high half period.
//   4. SCK low.
//
// With a protocol::Background, the engine calls Background::service()
// between bytes and in its waits, at most once per backgroundIntervalMs
// (protocol/Background.h). SCK is then low.

#include <array>
#include <cstdint>

#include "ClockPacer.h"
#include "jtag/JtagBitBang.h"
#include "protocol/Background.h"

namespace avr {

// The SPI divider is 2^shift (SPI_CTLR1.BR + 1). Shift 1 (36 MHz) is
// faster than any AVR allows: SCK must be below a quarter of the target
// clock (20 MHz at most).
inline constexpr uint8_t spiMinShift = 2;
inline constexpr uint8_t spiMaxShift = 8;

// The SPI divider shift for a request of kHz: the fastest SPI rate that is
// not above the request. 0: bit-bang, the request is below the slowest SPI
// rate (or 0).
constexpr uint8_t spiShiftFor(uint32_t spiClockHz, uint32_t kHz) {
   const uint64_t requestHz = uint64_t{ kHz } * 1000u;
   for(uint8_t shift = spiMinShift; shift <= spiMaxShift; ++shift) {
      if((spiClockHz >> shift) <= requestHz) {
         return shift;
      }
   }
   return 0;
}

// The time from the assert of RESET to the positive pulse, and the length of
// the pulse. The AVR datasheets ask for a pulse of at least two target
// clock cycles after SCK is low: 62.5 us at 32 kHz.
inline constexpr uint32_t resetPulseUs = 100;

// The parameters of the programming enable: the fields of
// CMD_ENTER_PROGMODE_ISP (AVR068), without the timeout.
struct EnableParams {
   // The wait after RESET, before the first instruction (ms)
   uint8_t stabDelayMs;
   // The wait after each attempt (ms)
   uint8_t cmdexeDelayMs;
   // The attempts. 0 is one attempt.
   uint8_t synchLoops;
   // The wait between the bytes of the instruction (ms)
   uint8_t byteDelayMs;
   // The response byte that shows the sync
   uint8_t pollValue;
   // 1-4: the response byte to compare with pollValue. 0: no check.
   uint8_t pollIndex;
   // The programming enable instruction (0xAC 0x53 0x00 0x00)
   std::array<uint8_t, 4> command;
};

using Instruction = std::array<uint8_t, 4>;

template <typename Pins, typename Spi>
class IspEngine {
   public:
   // background: serviced in the waits and between bytes, nullptr for none.
   explicit IspEngine(Pins &pins, protocol::Background *background = nullptr)
      : pins_(pins)
      , background_(background) {
   }

   // Sets the SCK frequency. Returns false for 0 (no change).
   [[nodiscard]] bool setSpeed(uint32_t kHz) {
      if(kHz == 0) {
         return false;
      }
      kHz_ = kHz;
      shift_ = spiShiftFor(spi_.clockHz(), kHz);
      if(shift_ != 0) {
         spi_.setShift(shift_);
      }
      return true;
   }

   // Drives the pins: SCK and MOSI low, RESET asserted.
   void connect() {
      pins_.init();
      spi_.init();
   }

   // Releases RESET and the pins: the target runs.
   void disconnect() {
      spi_.fini();
      pins_.fini();
   }

   void reset(bool asserted) {
      pins_.reset(asserted);
   }

   // Sends bytes and receives the same count. False on a hardware fault.
   [[nodiscard]] bool transfer(const uint8_t *out, uint8_t *in, uint32_t bytes) {
      begin();
      if(shift_ != 0) {
         static_cast<void>(background_.poll(pins_));
         return spi_.transfer(pins_, out, in, bytes);
      }
      for(uint32_t i = 0; i < bytes; ++i) {
         static_cast<void>(background_.poll(pins_));
         in[i] = bitBangByte(out[i]);
      }
      return true;
   }

   // One 4-byte instruction of the serial programming algorithm, with
   // byteDelayMs between the bytes. False on a hardware fault.
   [[nodiscard]] bool instruction(const Instruction &command, Instruction &response, uint8_t byteDelayMs = 0) {
      if(byteDelayMs == 0) {
         return transfer(command.data(), response.data(), 4);
      }
      for(std::size_t i = 0; i < command.size(); ++i) {
         if(i != 0) {
            waitMs(byteDelayMs);
         }
         if(!transfer(&command[i], &response[i], 1)) {
            return false;
         }
      }
      return true;
   }

   // Enables the programming mode, after connect(). A positive pulse on
   // RESET with SCK low, the wait stabDelayMs, then up to synchLoops
   // attempts. When the response byte at pollIndex is not pollValue, the
   // target is out of sync: one SCK pulse shifts its frame by one bit, and
   // the next attempt follows. Returns false when no attempt has the sync,
   // or on a hardware fault.
   [[nodiscard]] bool enableProgramming(const EnableParams &params) {
      begin();
      pins_.sck(false);
      pins_.reset(true);
      waitUs(resetPulseUs);
      pins_.reset(false);
      waitUs(resetPulseUs);
      pins_.reset(true);
      waitMs(params.stabDelayMs);
      const uint32_t attempts = params.synchLoops == 0 ? 1u : params.synchLoops;
      for(uint32_t attempt = 0; attempt < attempts; ++attempt) {
         if(attempt != 0) {
            pulseSck();
         }
         Instruction response{};
         if(!instruction(params.command, response, params.byteDelayMs)) {
            return false;
         }
         waitMs(params.cmdexeDelayMs);
         if(params.pollIndex == 0 || (params.pollIndex <= 4 && response[params.pollIndex - 1u] == params.pollValue)) {
            return true;
         }
      }
      return false;
   }

   // One positive SCK pulse at the set speed.
   void pulseSck() {
      begin();
      pacing::ClockPacer pacer;
      pacer.setHalfCycles(halfCycles_);
      pacer.start(pins_);
      pins_.sck(true);
      pacer.wait(pins_);
      pins_.sck(false);
      pacer.wait(pins_);
   }

   void waitMs(uint32_t ms) {
      for(uint32_t i = 0; i < ms; ++i) {
         waitUs(1000);
      }
   }

   // us up to about 29 s at 144 MHz (the 32-bit counter).
   void waitUs(uint32_t us) {
      begin();
      const uint32_t cycles = pins_.coreHz() / 1'000'000u * us;
      const uint32_t start = pins_.now();
      while(pins_.now() - start < cycles) {
         static_cast<void>(background_.poll(pins_));
      }
   }

   // The SCK frequency of the last setSpeed() (kHz)
   [[nodiscard]] uint32_t speedKHz() const {
      return kHz_;
   }

   // The SPI divider shift, 0 for bit-bang
   [[nodiscard]] uint8_t spiShift() const {
      return shift_;
   }

   // Core cycles of a half SCK period of bit-bang
   [[nodiscard]] uint32_t halfCycles() {
      return jtag::halfPeriodCycles(pins_.coreHz(), kHz_);
   }

   [[nodiscard]] Spi &spi() {
      return spi_;
   }

   private:
   // The core clock can change after the constructor (clock setup), so
   // the half period comes from it here.
   void begin() {
      pins_.begin();
      background_.begin(pins_);
      halfCycles_ = halfCycles();
   }

   uint8_t bitBangByte(uint8_t out) {
      pacing::ClockPacer pacer;
      pacer.setHalfCycles(halfCycles_);
      pacer.start(pins_);
      uint32_t in = 0;
      for(uint32_t bit = 0; bit < 8; ++bit) {
         pins_.mosi((out & 0x80u) != 0);
         out = static_cast<uint8_t>(out << 1);
         pacer.wait(pins_);
         in = (in << 1) | static_cast<uint32_t>(pins_.miso());
         pins_.sck(true);
         pacer.edge(pins_);
         pacer.wait(pins_);
         pins_.sck(false);
         pacer.edge(pins_);
      }
      return static_cast<uint8_t>(in);
   }

   Pins &pins_;
   Spi spi_;
   protocol::BackgroundTimer background_;
   // Until setSpeed(): bit-bang at 100 kHz, slow enough for a 1 MHz
   // target.
   uint32_t kHz_{ 100 };
   uint8_t shift_{ 0 };
   uint32_t halfCycles_{ 0 };
};

} // namespace avr

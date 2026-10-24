// The emulated TAP of the CMSIS-DAP RISC-V bridge (RvDtmTap.h): the TAP
// state machine, IR, IDCODE, DTMCS and BYPASS on their own, then the B0
// captures of OpenOCD and probe-rs (tests/replay/dap-*.trace) through the
// CMSIS-DAP processor with the TAP as its JTAG port. A host-side scan log
// collects the IR and DR scans from the requests and the TDO bits of the
// responses. The tests apply the scan chain rules of the host tools to these
// scans.

#include "protocol/cmsisdap/DapProcessor.h"
#include "protocol/cmsisdap/RvDtmTap.h"

#include "CmsisDapBench.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using cmsisdap::RvDtmTap;
using State = RvDtmTap::State;
using Bytes = std::vector<uint8_t>;
using Bits = std::vector<bool>;

// The bits of a string, first bit first: "0110"
Bits bits(const std::string &text) {
   Bits result;
   for(const char c : text) {
      result.push_back(c == '1');
   }
   return result;
}

Bits ones(std::size_t count) {
   return Bits(count, true);
}

Bits zeros(std::size_t count) {
   return Bits(count, false);
}

Bits operator+(Bits a, const Bits &b) {
   a.insert(a.end(), b.begin(), b.end());
   return a;
}

uint32_t value(const Bits &b, std::size_t first, std::size_t count) {
   uint32_t result = 0;
   for(std::size_t i = 0; i < count; ++i) {
      if(b.at(first + i)) {
         result |= 1u << i;
      }
   }
   return result;
}

// The LSB-first bits of a value
Bits valueBits(uint32_t v, std::size_t count) {
   Bits result;
   for(std::size_t i = 0; i < count; ++i) {
      result.push_back(((v >> i) & 1u) != 0);
   }
   return result;
}

// Shifts the bits through the TAP, TDI low when tdi is empty. Returns TDO.
Bits clock(RvDtmTap &tap, const Bits &tms, Bits tdi = {}) {
   tdi.resize(tms.size(), false);
   Bytes tmsBytes((tms.size() + 7) / 8);
   Bytes tdiBytes(tmsBytes.size());
   Bytes tdoBytes(tmsBytes.size());
   for(std::size_t i = 0; i < tms.size(); ++i) {
      tmsBytes[i / 8] = static_cast<uint8_t>(tmsBytes[i / 8] | (tms[i] ? 1u << (i % 8) : 0u));
      tdiBytes[i / 8] = static_cast<uint8_t>(tdiBytes[i / 8] | (tdi[i] ? 1u << (i % 8) : 0u));
   }
   REQUIRE(tap.shift(static_cast<uint32_t>(tms.size()), tdiBytes, tmsBytes, tdoBytes));
   Bits tdo;
   for(std::size_t i = 0; i < tms.size(); ++i) {
      tdo.push_back(((tdoBytes[i / 8] >> (i % 8)) & 1u) != 0);
   }
   return tdo;
}

// From Run-Test/Idle: a scan of the given TDI bits, back to Run-Test/Idle.
// Returns the TDO bits of the scan.
Bits scan(RvDtmTap &tap, bool ir, const Bits &tdi) {
   REQUIRE(tap.state() == State::Idle);
   clock(tap, ir ? bits("1100") : bits("100"));
   Bits tms = zeros(tdi.size());
   tms.back() = true;
   const Bits tdo = clock(tap, tms, tdi);
   clock(tap, bits("10"));
   return tdo;
}

// Test-Logic-Reset, then Run-Test/Idle
void reset(RvDtmTap &tap) {
   clock(tap, bits("111110"));
   REQUIRE(tap.state() == State::Idle);
}

} // namespace

TEST_CASE("RV DTM TAP: the DTMCS value") {
   // version 1 (debug spec 0.13), abits 7, idle 0, dmistat 0
   CHECK(cmsisdap::rvDtm::dtmcs == 0x00000071);
   // A valid IDCODE: bit 0 is 1, and it is not the end-of-chain value.
   CHECK((cmsisdap::rvDtm::idcode & 1u) == 1u);
   CHECK(cmsisdap::rvDtm::idcode != 0xFFFFFFFFu);
}

TEST_CASE("RV DTM TAP: five TMS high bits reach Test-Logic-Reset from each state") {
   // The TMS paths from Test-Logic-Reset to each state
   const std::vector<std::pair<std::string, State>> paths{
      { "", State::Reset },          { "0", State::Idle },         { "01", State::SelectDr },
      { "010", State::CaptureDr },   { "0100", State::ShiftDr },   { "0101", State::Exit1Dr },
      { "01010", State::PauseDr },   { "010101", State::Exit2Dr }, { "01011", State::UpdateDr },
      { "011", State::SelectIr },    { "0110", State::CaptureIr }, { "01100", State::ShiftIr },
      { "01101", State::Exit1Ir },   { "011010", State::PauseIr }, { "0110101", State::Exit2Ir },
      { "011011", State::UpdateIr },
   };
   for(const auto &[path, state] : paths) {
      CAPTURE(path);
      RvDtmTap tap;
      clock(tap, bits("11111" + path));
      CHECK(tap.state() == state);
      clock(tap, ones(5));
      CHECK(tap.state() == State::Reset);
      CHECK(tap.ir() == cmsisdap::rvDtm::irIdcode);
   }
}

TEST_CASE("RV DTM TAP: Test-Logic-Reset selects IDCODE") {
   RvDtmTap tap;
   reset(tap);
   // IDCODE, then the TDI bits 32 bits later
   const Bits tdo = scan(tap, false, zeros(32) + ones(32));
   CHECK(value(tdo, 0, 32) == cmsisdap::rvDtm::idcode);
   CHECK(tdo == valueBits(cmsisdap::rvDtm::idcode, 32) + zeros(32));
}

TEST_CASE("RV DTM TAP: IR capture is 00001 and the IR has 5 bits") {
   RvDtmTap tap;
   reset(tap);
   const Bits tdo = scan(tap, true, bits("10101") + ones(3));
   CHECK(tdo == bits("10000") + bits("101"));
   // The IR keeps the last 5 bits of TDI: 0x1E selects BYPASS.
   CHECK(tap.ir() == 0x1E);
}

TEST_CASE("RV DTM TAP: DTMCS") {
   RvDtmTap tap;
   reset(tap);
   scan(tap, true, valueBits(cmsisdap::rvDtm::irDtmcs, 5));
   CHECK(tap.ir() == cmsisdap::rvDtm::irDtmcs);
   CHECK(value(scan(tap, false, zeros(32)), 0, 32) == cmsisdap::rvDtm::dtmcs);
   // A write (dmireset) has no effect.
   scan(tap, false, valueBits(1u << 16, 32));
   CHECK(value(scan(tap, false, zeros(32)), 0, 32) == cmsisdap::rvDtm::dtmcs);
}

TEST_CASE("RV DTM TAP: BYPASS is 1 bit and captures 0") {
   for(const uint8_t ir : { cmsisdap::rvDtm::irBypass, cmsisdap::rvDtm::irDmi, uint8_t{ 0x00 }, uint8_t{ 0x05 } }) {
      CAPTURE(int{ ir });
      RvDtmTap tap;
      reset(tap);
      scan(tap, true, valueBits(ir, 5));
      CHECK(tap.ir() == ir);
      CHECK(scan(tap, false, bits("1101")) == bits("0110"));
   }
}

TEST_CASE("RV DTM TAP: Pause-DR keeps the shift register") {
   RvDtmTap tap;
   reset(tap);
   // Shift-DR: 10 bits, Exit1-DR, Pause-DR, Exit2-DR, Shift-DR: 22 bits
   clock(tap, bits("100"));
   Bits tdo = clock(tap, zeros(9) + bits("1"));
   clock(tap, bits("0010"));
   CHECK(tap.state() == State::ShiftDr);
   tdo = tdo + clock(tap, zeros(21) + bits("1"));
   CHECK(tap.state() == State::Exit1Dr);
   CHECK(value(tdo, 0, 32) == cmsisdap::rvDtm::idcode);
}

TEST_CASE("RV DTM TAP: TDO is high outside the shift states") {
   RvDtmTap tap;
   // Test-Logic-Reset, Run-Test/Idle, Select-DR-Scan, Capture-DR
   CHECK(clock(tap, bits("11110010")) == ones(8));
   CHECK(tap.state() == State::CaptureDr);
}

TEST_CASE("RV DTM TAP: an IR scan through Pause-IR updates the IR once") {
   RvDtmTap tap;
   reset(tap);
   // Select-DR-Scan, Select-IR-Scan, Capture-IR, Shift-IR: 2 bits,
   // Exit1-IR, Pause-IR, Exit2-IR, Shift-IR: 3 bits, Exit1-IR, Update-IR
   const Bits dtmcs = valueBits(cmsisdap::rvDtm::irDtmcs, 5);
   clock(tap, bits("1100"));
   clock(tap, bits("01"), { dtmcs[0], dtmcs[1] });
   clock(tap, bits("010"));
   CHECK(tap.ir() == cmsisdap::rvDtm::irIdcode);
   clock(tap, bits("001"), { dtmcs[2], dtmcs[3], dtmcs[4] });
   clock(tap, bits("10"));
   CHECK(tap.ir() == cmsisdap::rvDtm::irDtmcs);
}

namespace {

// The host view of the JTAG stream: it follows TMS and collects the IR and
// DR scans. A scan starts at the first bit in Shift-IR or Shift-DR and ends
// at Update-IR, Update-DR or Test-Logic-Reset. It goes on through Pause.
class ScanLog {
   public:
   struct Scan {
      bool ir;
      Bits tdi;
      Bits tdo;
      // True when the bit has the TDO capture flag of DAP_JTAG_Sequence
      std::vector<bool> captured;
   };

   void clock(bool tms, bool tdi, bool tdo, bool captured) {
      if(state_ == State::ShiftDr || state_ == State::ShiftIr) {
         if(!open_) {
            scans.push_back({ state_ == State::ShiftIr, {}, {}, {} });
            open_ = true;
         }
         scans.back().tdi.push_back(tdi);
         scans.back().tdo.push_back(tdo);
         scans.back().captured.push_back(captured);
      }
      state_ = next(state_, tms);
      if(state_ == State::UpdateDr || state_ == State::UpdateIr || state_ == State::Reset) {
         open_ = false;
      }
   }

   [[nodiscard]] State state() const {
      return state_;
   }

   std::vector<Scan> scans;

   private:
   static State next(State s, bool tms) {
      switch(s) {
      case State::Reset:
         return tms ? State::Reset : State::Idle;
      case State::Idle:
      case State::UpdateDr:
      case State::UpdateIr:
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

   State state_{ State::Reset };
   bool open_{ false };
};

// A platform whose clock runs: DAP_SWJ_Pins waits end.
class RunningPlatform final : public cmsisdap::Platform {
   public:
   uint32_t nowUs() override {
      return now_++;
   }
   void delayUs(uint32_t) override {
   }
   void setLed(cmsisdap::Led, bool) override {
   }

   private:
   uint32_t now_{ 0 };
};

bool bitOf(const Bytes &bytes, std::size_t offset, uint32_t index) {
   return ((bytes.at(offset + index / 8) >> (index % 8)) & 1u) != 0;
}

// The CMSIS-DAP processor with the emulated TAP as its JTAG port
struct Bridge {
   Bridge()
      : info(vsltest::dapTestInfo())
      , dap(swd, gpio, platform, info, &tap) {
   }

   // Runs one request, and adds its JTAG bits to the scan log
   Bytes run(const Bytes &request) {
      REQUIRE(dap.canSubmit());
      dap.submit(request);
      dap.run();
      REQUIRE(dap.hasResponse());
      const auto span = dap.response();
      Bytes response(span.begin(), span.end());
      dap.popResponse();
      if(request.at(0) == 0x12) {
         REQUIRE(response == Bytes{ 0x12, 0x00 });
         const uint32_t count = request.at(1) == 0 ? 256 : request.at(1);
         for(uint32_t i = 0; i < count; ++i) {
            log.clock(bitOf(request, 2, i), true, true, false);
         }
      } else if(request.at(0) == 0x14) {
         REQUIRE(response.size() >= 2);
         REQUIRE(response[1] == 0x00);
         std::size_t in = 2;
         std::size_t out = 2;
         for(uint8_t entry = 0; entry < request.at(1); ++entry) {
            const uint8_t entryInfo = request.at(in++);
            const uint32_t count = (entryInfo & 0x3Fu) == 0 ? 64 : (entryInfo & 0x3Fu);
            const bool tms = (entryInfo & 0x40u) != 0;
            const bool capture = (entryInfo & 0x80u) != 0;
            for(uint32_t i = 0; i < count; ++i) {
               log.clock(tms, bitOf(request, in, i), capture && bitOf(response, out, i), capture);
            }
            in += (count + 7) / 8;
            out += capture ? (count + 7) / 8 : 0;
         }
         REQUIRE(out == response.size());
      }
      return response;
   }

   // Runs the requests of a trace: one "> hex" line for each request
   void replay(const std::string &name) {
      std::ifstream file(std::string(VSL_REPLAY_DIR) + "/" + name);
      REQUIRE(file.is_open());
      std::string line;
      while(std::getline(file, line)) {
         if(line.empty() || line[0] != '>') {
            continue;
         }
         std::istringstream hex(line.substr(1));
         std::string text;
         hex >> text;
         Bytes request;
         for(std::size_t i = 0; i + 1 < text.size(); i += 2) {
            request.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
         }
         CAPTURE(line);
         run(request);
      }
   }

   RvDtmTap tap;
   vsltest::FakeDapSwd swd;
   vsltest::FakeDapGpio gpio;
   RunningPlatform platform;
   cmsisdap::DeviceInfo info;
   cmsisdap::Processor dap;
   ScanLog log;
};

// jtag_examine_chain() of OpenOCD (src/jtag/core.c): the IDCODE of each
// device, or 0 for a device in BYPASS. Fails on an all-zeros or all-ones
// scan, and on bits after the end of the chain that are not ones.
std::vector<uint32_t> openocdExamineChain(const ScanLog::Scan &scan) {
   const Bits &tdo = scan.tdo;
   REQUIRE(tdo.size() % 32 == 0);
   bool zeros = true;
   bool allOnes = true;
   for(const bool b : tdo) {
      zeros = zeros && !b;
      allOnes = allOnes && b;
   }
   REQUIRE_FALSE(zeros);
   REQUIRE_FALSE(allOnes);
   std::vector<uint32_t> devices;
   std::size_t bit = 0;
   while(bit + 32 <= tdo.size()) {
      const uint32_t idcode = value(tdo, bit, 32);
      if(idcode == 0xFFFFFFFFu) {
         break;
      }
      if((idcode & 1u) == 0) {
         devices.push_back(0);
         bit += 1;
      } else {
         devices.push_back(idcode);
         bit += 32;
      }
   }
   // jtag_examine_chain_end()
   for(; bit + 32 <= tdo.size(); bit += 32) {
      CHECK(value(tdo, bit, 32) == 0xFFFFFFFFu);
   }
   return devices;
}

// jtag_validate_ircapture() of OpenOCD with one TAP of irLength bits:
// the two low bits of the captured IR are 01, and the 2-bit sentinel of
// ones follows the IR.
void openocdValidateIrCapture(const ScanLog::Scan &scan, std::size_t irLength) {
   REQUIRE(scan.ir);
   REQUIRE(scan.tdo.size() == irLength + 2);
   CHECK((value(scan.tdo, 0, irLength) & 0x3u) == 0x1u);
   CHECK(value(scan.tdo, irLength, 2) == 0x3u);
}

// The output of a DR scan through one device with a register of length bits
// that captured capture: the captured value, then TDI, length bits later.
// Only the captured bits count.
void checkDelay(const ScanLog::Scan &scan, uint32_t capture, std::size_t length) {
   REQUIRE_FALSE(scan.ir);
   for(std::size_t i = 0; i < scan.tdo.size(); ++i) {
      if(!scan.captured[i]) {
         continue;
      }
      CAPTURE(i);
      const bool expected = i < length ? ((capture >> i) & 1u) != 0 : scan.tdi[i - length];
      CHECK(scan.tdo[i] == expected);
   }
}

} // namespace

TEST_CASE("RV DTM TAP: the OpenOCD capture finds the DTM") {
   Bridge b;
   b.replay("dap-openocd-riscv.trace");
   const auto &scans = b.log.scans;
   REQUIRE(scans.size() == 5);

   // The IDCODE scan of 672 bits, from Test-Logic-Reset
   REQUIRE_FALSE(scans[0].ir);
   REQUIRE(scans[0].tdo.size() == 672);
   CHECK(openocdExamineChain(scans[0]) == std::vector<uint32_t>{ cmsisdap::rvDtm::idcode });

   // The IR capture check: 5 bits of the TAP, then 2 bits of sentinel
   openocdValidateIrCapture(scans[1], cmsisdap::rvDtm::irLength);

   // riscv-013 examine: DTMCS, then the DMI instruction
   CHECK(scans[2].ir);
   CHECK(value(scans[2].tdi, 0, 5) == cmsisdap::rvDtm::irDtmcs);
   CHECK_FALSE(scans[3].ir);
   REQUIRE(scans[3].tdo.size() == 32);
   const uint32_t dtmcs = value(scans[3].tdo, 0, 32);
   CHECK((dtmcs & 0xFu) == 1u);
   CHECK(((dtmcs >> 4) & 0x3Fu) == 7u);
   CHECK(value(scans[4].tdi, 0, 5) == cmsisdap::rvDtm::irDmi);

   CHECK(b.tap.state() == State::Idle);
   CHECK(b.log.state() == State::Idle);
   CHECK(b.tap.ir() == cmsisdap::rvDtm::irDmi);
}

TEST_CASE("RV DTM TAP: the probe-rs capture with a CH32V003 finds the DTM") {
   Bridge b;
   b.replay("dap-probe-rs-reset-ch32v003.trace");
   const auto &scans = b.log.scans;
   REQUIRE(scans.size() == 2);
   for(const auto &scan : scans) {
      // 256 bits of ones: the IDCODE, then the ones of TDI
      REQUIRE(scan.tdo.size() == 256);
      checkDelay(scan, cmsisdap::rvDtm::idcode, 32);
      CHECK(openocdExamineChain(scan) == std::vector<uint32_t>{ cmsisdap::rvDtm::idcode });
   }
   // Test-Logic-Reset, then Run-Test/Idle
   CHECK(b.tap.state() == State::Idle);
   CHECK(b.log.state() == State::Idle);
}

TEST_CASE("RV DTM TAP: the probe-rs info capture finds the DTM and a DR length of 32") {
   Bridge b;
   b.replay("dap-probe-rs-info.trace");
   const auto &scans = b.log.scans;
   REQUIRE(scans.size() == 5);
   for(const std::size_t i : { 0, 1, 3, 4 }) {
      CAPTURE(i);
      REQUIRE(scans[i].tdo.size() == 256);
      checkDelay(scans[i], cmsisdap::rvDtm::idcode, 32);
   }
   // 128 zeros, then 129 ones. The last bit has no TDO capture. The first
   // one after the zeros comes 32 bits after the first one of TDI.
   const auto &length = scans[2];
   REQUIRE(length.tdo.size() == 257);
   CHECK_FALSE(length.captured.back());
   checkDelay(length, cmsisdap::rvDtm::idcode, 32);
   CHECK(value(length.tdo, 128, 32) == 0u);
   CHECK(value(length.tdo, 160, 32) == 0xFFFFFFFFu);
   // Test-Logic-Reset, then Run-Test/Idle
   CHECK(b.tap.state() == State::Idle);
   CHECK(b.log.state() == State::Idle);
}

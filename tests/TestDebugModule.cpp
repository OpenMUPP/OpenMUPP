// SPDX-License-Identifier: GPL-3.0-or-later
// The debug module layer on the wire engine, with mock pins and a mock DM.
// The reference access sequences come from the WCH-LinkE captures in
// docs/wch-link-captures (docs/wch-link-protocol.md, "Probe actions").

#include "rvswd/DebugModule.h"

#include "MockDebugModule.h"
#include "MockRvSwdTarget.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using mupptest::MockDebugModule;
using mupptest::MockRvSwdTarget;
using rvswd::DebugModule;
using rvswd::MemoryAccess;
using rvswd::RvSwdEngine;
using rvswd::Status;
using rvswd::Width;
using rvswd::Wire;

namespace dm = rvswd::dm;

struct Access {
   uint8_t address;
   bool write;
   uint32_t data;

   bool operator==(const Access &other) const {
      return address == other.address && write == other.write && data == other.data;
   }
};

Access W(uint8_t address, uint32_t data) {
   return { address, true, data };
}
Access R(uint8_t address, uint32_t data) {
   return { address, false, data };
}

struct Bench {
   explicit Bench(Wire wire)
      : pins(wire)
      , target(pins)
      , engine(pins)
      , module(engine) {
      engine.init(wire);
      module.init();
   }

   // The DM is active and the hart halted, as after the connect command.
   void connect() {
      REQUIRE(module.halt() == Status::Ok);
      pins.accesses.clear();
   }

   // The accesses since the last clear, and a new clear
   std::vector<Access> take() {
      std::vector<Access> result;
      for(const auto &access : pins.accesses) {
         result.push_back({ access.address, access.write, access.data });
      }
      pins.accesses.clear();
      return result;
   }

   void checkWire() const {
      CHECK(pins.protocolErrors == 0);
      CHECK(pins.contentions == 0);
   }

   MockRvSwdTarget pins;
   MockDebugModule target;
   RvSwdEngine<MockRvSwdTarget> engine;
   DebugModule<RvSwdEngine<MockRvSwdTarget>> module;
};

const Wire wires[] = { Wire::One, Wire::Two };

} // namespace

// "05 W 1ffff7e0, 17 W 02200000, ..." in a failed check
template <>
struct doctest::StringMaker<std::vector<Access>> {
   static String convert(const std::vector<Access> &accesses) {
      std::string text;
      for(const Access &access : accesses) {
         char item[24];
         std::snprintf(item, sizeof(item), "%02x %c %08x, ", access.address, access.write ? 'W' : 'R', access.data);
         text += item;
      }
      return text.c_str();
   }
};

TEST_CASE("The memory access method follows the wire") {
   CHECK(Bench(Wire::One).module.memoryAccess() == MemoryAccess::ProgramBuffer);
   CHECK(Bench(Wire::Two).module.memoryAccess() == MemoryAccess::Abstract);
   Bench bench(Wire::Two);
   bench.module.setMemoryAccess(MemoryAccess::ProgramBuffer);
   CHECK(bench.module.memoryAccess() == MemoryAccess::ProgramBuffer);
}

TEST_CASE("DMI read and write go to the DM register unchanged") {
   for(const Wire wire : wires) {
      CAPTURE(wire);
      Bench bench(wire);
      bench.pins.registers[dm::chipId] = 0x2080'053c;
      uint32_t value = 0;
      CHECK(bench.module.dmiRead(dm::chipId, value) == Status::Ok);
      CHECK(value == 0x2080'053c);
      CHECK(bench.module.dmiWrite(0x7e, 0x5aa5'0400) == Status::Ok);
      CHECK(bench.pins.registers[0x7e] == 0x5aa5'0400);
      CHECK(bench.take() == std::vector{ R(dm::chipId, 0x2080'053c), W(0x7e, 0x5aa5'0400) });
      bench.checkWire();
   }
}

TEST_CASE("Two wires: a frame without a target gives NoTarget") {
   Bench bench(Wire::Two);
   bench.pins.present = false;
   uint32_t value = 0;
   CHECK(bench.module.dmiWrite(dm::data0, 1) == Status::NoTarget);
   CHECK(bench.module.dmiRead(dm::dmstatus, value) == Status::NoTarget);
   CHECK(bench.module.halt() == Status::NoTarget);
}

TEST_CASE("Two wires: a frame that the target drops gives Wire") {
   // The target drives bit 11, then no more bits (the CH32V208 with a
   // released bit 13). The write is lost, without a parity error.
   Bench bench(Wire::Two);
   bench.pins.dropFrames = true;
   uint32_t value = 0;
   CHECK(bench.module.dmiWrite(dm::data0, 1) == Status::Wire);
   CHECK(bench.module.dmiRead(dm::dmstatus, value) == Status::Wire);
   CHECK(bench.pins.droppedFrames == 2);
   CHECK(bench.pins.registers[dm::data0] == 0);
}

TEST_CASE("One wire: dmstatus without a target gives NoTarget") {
   Bench bench(Wire::One);
   bench.pins.present = false;
   uint32_t value = 0;
   // The frames themselves have no error: the line reads 1.
   CHECK(bench.module.dmiRead(dm::dmstatus, value) == Status::Ok);
   CHECK(value == 0xFFFF'FFFFu);
   CHECK(bench.module.readDmstatus(value) == Status::NoTarget);
   CHECK(bench.module.halt() == Status::NoTarget);
   CHECK(bench.module.resetDm() == Status::NoTarget);
}

TEST_CASE("The flags of the engine give a wire error") {
   SUBCASE("Two wires: parity") {
      Bench bench(Wire::Two);
      bench.pins.badParity = true;
      uint32_t value = 0;
      CHECK(bench.module.dmiRead(dm::dmstatus, value) == Status::Wire);
   }
   SUBCASE("One wire: line low") {
      Bench bench(Wire::One);
      bench.pins.stuckLow = true;
      uint32_t value = 0;
      CHECK(bench.module.dmiRead(dm::dmstatus, value) == Status::Wire);
   }
}

TEST_CASE("DM reset: dmcontrol 0, then dmactive (wlink halt)") {
   for(const Wire wire : wires) {
      CAPTURE(wire);
      Bench bench(wire);
      CHECK(bench.module.resetDm() == Status::Ok);
      CHECK(bench.target.active());
      CHECK(bench.take() ==
            std::vector{ W(dm::dmcontrol, 0), W(dm::dmcontrol, 1), R(dm::dmcontrol, 1), R(dm::dmstatus, 0x0000'0c82) });
      bench.checkWire();
   }
}

TEST_CASE("Halt: haltreq twice, then dmstatus (the connect of the probe)") {
   for(const Wire wire : wires) {
      CAPTURE(wire);
      Bench bench(wire);
      bool halted = true;
      CHECK(bench.module.halted(halted) == Status::Ok);
      CHECK_FALSE(halted);
      bench.take();
      CHECK(bench.module.halt() == Status::Ok);
      CHECK(bench.target.halted);
      CHECK(bench.take() == std::vector{ W(dm::dmcontrol, 0x8000'0001), W(dm::dmcontrol, 0x8000'0001), R(dm::dmstatus, 0x0000'0382) });
      // haltreq stays set, as on the probe.
      uint32_t control = 0;
      CHECK(bench.module.dmiRead(dm::dmcontrol, control) == Status::Ok);
      CHECK(control == 0x8000'0001);
      CHECK(bench.module.halted(halted) == Status::Ok);
      CHECK(halted);
      bench.checkWire();
   }
}

TEST_CASE("Halt: a hart that does not halt gives Timeout") {
   Bench bench(Wire::Two);
   bench.target.neverHalts = true;
   CHECK(bench.module.halt() == Status::Timeout);
   const auto accesses = bench.take();
   CHECK(accesses.size() == 2 + rvswd::pollLimit);
}

TEST_CASE("Resume: clear haltreq, resumereq, then dmstatus (wlink resume)") {
   for(const Wire wire : wires) {
      CAPTURE(wire);
      Bench bench(wire);
      bench.connect();
      CHECK(bench.module.resume() == Status::Ok);
      CHECK_FALSE(bench.target.halted);
      CHECK(bench.take() == std::vector{ W(dm::dmcontrol, 0x0000'0001), W(dm::dmcontrol, 0x4000'0001), R(dm::dmstatus, 0x0003'0c82) });
      // The first write cleared haltreq: the CH32V006 ignores a resumereq in the same write.
      CHECK(bench.pins.registers[dm::dmcontrol] == 0x0000'0001);
      bench.checkWire();
   }
}

TEST_CASE("Abstract register access") {
   for(const Wire wire : wires) {
      CAPTURE(wire);
      Bench bench(wire);
      bench.connect();
      bench.target.csrs[0x301] = 0x4080'0014;

      uint32_t value = 0;
      CHECK(bench.module.readRegister(0x301, value) == Status::Ok);
      CHECK(value == 0x4080'0014);
      // The first command clears cmderr.
      CHECK(bench.take() == std::vector{ W(dm::abstractcs, 0x700), W(dm::command, 0x0022'0301),
                                         R(dm::abstractcs, 0x0800'0002), R(dm::data0, 0x4080'0014) });

      CHECK(bench.module.writeRegister(0x7c0, 0x300) == Status::Ok);
      CHECK(bench.target.csrs[0x7c0] == 0x300);
      CHECK(bench.take() == std::vector{ W(dm::data0, 0x300), W(dm::command, 0x0023'07c0), R(dm::abstractcs, 0x0800'0002) });

      CHECK(bench.module.writeRegister(dm::gpr0 + 10, 0x1234'5678) == Status::Ok);
      CHECK(bench.target.gprs[10] == 0x1234'5678);
      CHECK(bench.module.readRegister(dm::gpr0 + 10, value) == Status::Ok);
      CHECK(value == 0x1234'5678);
      bench.checkWire();
   }
}

TEST_CASE("A failed command gives Command and clears cmderr") {
   Bench bench(Wire::Two);
   CHECK(bench.module.resetDm() == Status::Ok);
   bench.take();
   uint32_t value = 0;
   // Not halted: cmderr 4
   CHECK(bench.module.readRegister(dm::dpc, value) == Status::Command);
   CHECK(bench.module.cmderr() == 4);
   CHECK(bench.target.cmderr() == 0);
   CHECK(bench.take() == std::vector{ W(dm::abstractcs, 0x700), W(dm::command, 0x0022'07b1),
                                      R(dm::abstractcs, 0x0800'0402), W(dm::abstractcs, 0x700) });
   // The next command runs.
   REQUIRE(bench.module.halt() == Status::Ok);
   bench.target.csrs[dm::dpc] = 0x0800'0100;
   CHECK(bench.module.readRegister(dm::dpc, value) == Status::Ok);
   CHECK(value == 0x0800'0100);
}

TEST_CASE("A cmderr from before the module is cleared before the first command") {
   // The CH32V003 shows cmderr 6 after the connect of the probe.
   Bench bench(Wire::One);
   bench.connect();
   bench.target.setCmderr(6);
   uint32_t value = 0;
   CHECK(bench.module.readRegister(dm::dpc, value) == Status::Ok);
   CHECK(bench.target.commands.size() == 1);
}

TEST_CASE("A DMI write of the host makes the module clear cmderr again") {
   Bench bench(Wire::Two);
   bench.connect();
   uint32_t value = 0;
   CHECK(bench.module.readRegister(dm::dpc, value) == Status::Ok);
   CHECK(bench.module.dmiWrite(dm::command, 0x0022'1234) == Status::Ok);
   CHECK(bench.target.cmderr() == 3);
   bench.take();
   CHECK(bench.module.readRegister(dm::dpc, value) == Status::Ok);
   CHECK(bench.take().front() == W(dm::abstractcs, 0x700));
}

TEST_CASE("A busy command is polled until it ends") {
   Bench bench(Wire::Two);
   bench.connect();
   SUBCASE("Ends") {
      bench.target.busyReads = 3;
      uint32_t value = 0;
      CHECK(bench.module.readRegister(dm::dpc, value) == Status::Ok);
      const auto accesses = bench.take();
      // cmderr clear, command, 4 abstractcs reads, data0
      CHECK(accesses.size() == 7);
   }
   SUBCASE("Does not end") {
      bench.target.busyReads = 1000;
      uint32_t value = 0;
      CHECK(bench.module.readRegister(dm::dpc, value) == Status::Timeout);
      CHECK(bench.take().size() == 2 + rvswd::pollLimit);
      // The next command clears cmderr first.
      bench.target.busyReads = 0;
      bench.module.readRegister(dm::dpc, value);
      CHECK(bench.take().front() == W(dm::abstractcs, 0x700));
   }
}

TEST_CASE("Abstract memory access (CH32V20x)") {
   Bench bench(Wire::Two);
   bench.connect();
   bench.target.store(0x1fff'f7e0, 0xffff'00a0, 4);
   bench.target.store(0x1fff'f802, 0x3f, 1);
   uint32_t value = 0;

   SUBCASE("Word read: the triple of the probe, with the abstractcs check") {
      CHECK(bench.module.readMemory(0x1fff'f7e0, value) == Status::Ok);
      CHECK(value == 0xffff'00a0);
      CHECK(bench.take() == std::vector{ W(dm::data1, 0x1fff'f7e0), W(dm::abstractcs, 0x700), W(dm::command, 0x0220'0000),
                                         R(dm::abstractcs, 0x0800'0002), R(dm::data0, 0xffff'00a0) });
   }
   SUBCASE("Byte read (the 0d 04 command of the probe)") {
      bench.target.store(0x1fff'f803, 0x55, 1);
      CHECK(bench.module.readMemory(0x1fff'f802, value, Width::Byte) == Status::Ok);
      CHECK(value == 0x3f);
      CHECK(bench.target.commands.back() == 0x0200'0000);
   }
   SUBCASE("Half word read") {
      bench.target.store(0x2000'0000, 0xa5a5'1234, 4);
      CHECK(bench.module.readMemory(0x2000'0000, value, Width::Half) == Status::Ok);
      CHECK(value == 0x1234);
   }
   SUBCASE("Word write") {
      CHECK(bench.module.readMemory(0x4002'1004, value) == Status::Ok);
      bench.take();
      CHECK(bench.module.writeMemory(0x4002'1004, 0x0035'0000) == Status::Ok);
      CHECK(bench.target.load(0x4002'1004, 4) == 0x0035'0000);
      CHECK(bench.take() == std::vector{ W(dm::data1, 0x4002'1004), W(dm::data0, 0x0035'0000),
                                         W(dm::command, 0x0221'0000), R(dm::abstractcs, 0x0800'0002) });
   }
   SUBCASE("Byte write") {
      bench.target.store(0x2000'0000, 0x1122'3344, 4);
      CHECK(bench.module.writeMemory(0x2000'0001, 0xab, Width::Byte) == Status::Ok);
      CHECK(bench.target.load(0x2000'0000, 4) == 0x1122'ab44);
   }
   SUBCASE("A running hart gives Command") {
      REQUIRE(bench.module.resume() == Status::Ok);
      CHECK(bench.module.readMemory(0x1fff'f7e0, value) == Status::Command);
      CHECK(bench.module.cmderr() == 4);
   }
   bench.checkWire();
}

TEST_CASE("Program buffer memory access (CH32V00x)") {
   Bench bench(Wire::One);
   bench.connect();
   bench.target.store(0x1fff'f7c4, 0x0030'0500, 4);
   // a0 and a1 of the program, as the hart had them
   bench.target.gprs[10] = 0xaaaa'0010;
   bench.target.gprs[11] = 0xaaaa'0011;
   uint32_t value = 0;

   // The first read loads the program of the probe.
   CHECK(bench.module.readMemory(0x1fff'f7c4, value) == Status::Ok);
   CHECK(value == 0x0030'0500);
   CHECK(bench.take() == std::vector{ W(dm::abstractauto, 0), W(0x20, 0x7b25'1073), W(0x21, 0x7b35'9073), W(0x22, 0xe000'0537),
                                      W(0x23, 0x0f85'2583), W(0x24, 0x2a23'418c), W(0x25, 0x2573'0eb5), W(0x26, 0x25f3'7b20),
                                      W(0x27, 0x9002'7b30), W(dm::data1, 0x1fff'f7c4), W(dm::abstractcs, 0x700),
                                      W(dm::command, 0x0004'0000), R(dm::abstractcs, 0x0800'0002), R(dm::data0, 0x0030'0500) });

   // The next read: the triple of the probe, with the abstractcs check
   bench.target.store(0x1fff'f7e0, 0xffff'0010, 4);
   CHECK(bench.module.readMemory(0x1fff'f7e0, value) == Status::Ok);
   CHECK(value == 0xffff'0010);
   CHECK(bench.take() == std::vector{ W(dm::data1, 0x1fff'f7e0), W(dm::command, 0x0004'0000),
                                      R(dm::abstractcs, 0x0800'0002), R(dm::data0, 0xffff'0010) });

   // A write loads the write program.
   CHECK(bench.module.writeMemory(0x2000'0010, 0xdead'beef) == Status::Ok);
   CHECK(bench.target.load(0x2000'0010, 4) == 0xdead'beef);
   auto accesses = bench.take();
   CHECK(accesses.size() == 1 + 8 + 4);
   CHECK(bench.module.writeMemory(0x2000'0014, 0x0102'0304) == Status::Ok);
   CHECK(bench.target.load(0x2000'0014, 4) == 0x0102'0304);
   CHECK(bench.take() == std::vector{ W(dm::data1, 0x2000'0014), W(dm::data0, 0x0102'0304), W(dm::command, 0x0004'0000),
                                      R(dm::abstractcs, 0x0800'0002) });

   CHECK(bench.module.readMemory(0x2000'0010, value) == Status::Ok);
   CHECK(value == 0xdead'beef);

   // The programs keep a0 and a1.
   CHECK(bench.target.gprs[10] == 0xaaaa'0010);
   CHECK(bench.target.gprs[11] == 0xaaaa'0011);
   CHECK(bench.target.programRuns == 5);
   bench.checkWire();
}

TEST_CASE("Program buffer: word reads, word and half-word writes only") {
   Bench bench(Wire::One);
   bench.connect();
   uint32_t value = 0;
   CHECK(bench.module.readMemory(0x2000'0000, value, Width::Byte) == Status::Unsupported);
   CHECK(bench.module.readMemory(0x2000'0000, value, Width::Half) == Status::Unsupported);
   CHECK(bench.module.writeMemory(0x2000'0000, 0, Width::Byte) == Status::Unsupported);
   CHECK(bench.take().empty());
}

TEST_CASE("Program buffer: a half-word write keeps a0 and a1 with register accesses") {
   Bench bench(Wire::One);
   bench.connect();
   bench.target.gprs[10] = 0xaaaa'0010;
   bench.target.gprs[11] = 0xaaaa'0011;
   bench.target.store(0x1fff'f800, 0xffff'ffff, 4);

   CHECK(bench.module.writeMemory(0x1fff'f802, 0x1234'00a5, Width::Half) == Status::Ok);
   // Only the half word changes.
   CHECK(bench.target.load(0x1fff'f800, 4) == 0x00a5'ffff);
   CHECK(bench.target.gprs[10] == 0xaaaa'0010);
   CHECK(bench.target.gprs[11] == 0xaaaa'0011);
   CHECK(bench.take() ==
         std::vector{ W(dm::abstractauto, 0), W(0x20, 0xe000'0537), W(0x21, 0x0f45'2583), W(0x22, 0x0f85'2503),
                      W(0x23, 0x00b5'1023), W(0x24, 0x0000'9002),
                      // Save a0 and a1
                      W(dm::abstractcs, 0x700), W(dm::command, 0x0022'100a), R(dm::abstractcs, 0x0800'0002), R(dm::data0, 0xaaaa'0010),
                      W(dm::command, 0x0022'100b), R(dm::abstractcs, 0x0800'0002), R(dm::data0, 0xaaaa'0011),
                      // The store
                      W(dm::data1, 0x1fff'f802), W(dm::data0, 0x0000'00a5), W(dm::command, 0x0004'0000), R(dm::abstractcs, 0x0800'0002),
                      // Restore a0 and a1
                      W(dm::data0, 0xaaaa'0010), W(dm::command, 0x0023'100a), R(dm::abstractcs, 0x0800'0002),
                      W(dm::data0, 0xaaaa'0011), W(dm::command, 0x0023'100b), R(dm::abstractcs, 0x0800'0002) });

   // The program stays loaded. A word write loads the word program.
   CHECK(bench.module.writeMemory(0x1fff'f800, 0x5aa5, Width::Half) == Status::Ok);
   CHECK(bench.take().size() == 6 + 4 + 6);
   CHECK(bench.module.writeMemory(0x2000'0000, 0x0102'0304) == Status::Ok);
   CHECK(bench.take().front() == W(dm::abstractauto, 0));
   bench.checkWire();
}

TEST_CASE("Program buffer: a fault in a half-word write restores a0 and a1") {
   Bench bench(Wire::One);
   bench.connect();
   bench.target.gprs[10] = 0xaaaa'0010;
   bench.target.gprs[11] = 0xaaaa'0011;
   bench.target.faultAddresses.insert(0x2000'0002);
   CHECK(bench.module.writeMemory(0x2000'0002, 0x1234, Width::Half) == Status::Command);
   CHECK(bench.module.cmderr() == rvswd::cmderrException);
   CHECK(bench.target.gprs[10] == 0xaaaa'0010);
   CHECK(bench.target.gprs[11] == 0xaaaa'0011);
   CHECK(bench.target.memory.count(0x2000'0002) == 0);
   // The hart must be halted for the register accesses.
   REQUIRE(bench.module.resume() == Status::Ok);
   CHECK(bench.module.writeMemory(0x2000'0000, 0x1234, Width::Half) == Status::Command);
   CHECK(bench.module.cmderr() == 4);
}

TEST_CASE("Program buffer: the program is loaded again after a DM reset or a host write") {
   Bench bench(Wire::One);
   bench.connect();
   uint32_t value = 0;
   CHECK(bench.module.readMemory(0x2000'0000, value) == Status::Ok);
   SUBCASE("DM reset") {
      CHECK(bench.module.resetDm() == Status::Ok);
   }
   SUBCASE("Host write") {
      CHECK(bench.module.dmiWrite(0x20, 0) == Status::Ok);
   }
   bench.take();
   CHECK(bench.module.readMemory(0x2000'0000, value) == Status::Ok);
   const auto accesses = bench.take();
   REQUIRE(accesses.size() > 1);
   CHECK(accesses[0] == W(dm::abstractauto, 0));
   CHECK(accesses[1] == W(0x20, 0x7b25'1073));
}

TEST_CASE("Program buffer: a program error gives Command") {
   Bench bench(Wire::Two);
   bench.connect();
   bench.module.setMemoryAccess(MemoryAccess::ProgramBuffer);
   uint32_t value = 0;
   CHECK(bench.module.readMemory(0x2000'0000, value) == Status::Ok);
   bench.target.failNext = 3;
   CHECK(bench.module.readMemory(0x2000'0000, value) == Status::Command);
   CHECK(bench.module.cmderr() == 3);
   bench.target.store(0x2000'0000, 0x55aa'55aa, 4);
   CHECK(bench.module.readMemory(0x2000'0000, value) == Status::Ok);
   CHECK(value == 0x55aa'55aa);
}

TEST_CASE("The write program is valid RV32EC code") {
   // The mock interpreter runs both programs. An unknown instruction or a
   // missing ebreak gives cmderr 3.
   Bench bench(Wire::Two);
   bench.connect();
   for(std::size_t i = 0; i < rvswd::programWords; ++i) {
      REQUIRE(bench.module.dmiWrite(static_cast<uint8_t>(dm::progbuf0 + i), rvswd::writeWordProgram[i]) == Status::Ok);
   }
   REQUIRE(bench.module.dmiWrite(dm::data1, 0x2000'0100) == Status::Ok);
   REQUIRE(bench.module.dmiWrite(dm::data0, 0x8765'4321) == Status::Ok);
   CHECK(bench.module.execute(dm::execute) == Status::Ok);
   CHECK(bench.target.load(0x2000'0100, 4) == 0x8765'4321);
}

TEST_CASE("Resume of a running hart ends at allrunning") {
   for(const Wire wire : wires) {
      CAPTURE(wire);
      Bench bench(wire);
      SUBCASE("After a DM reset") {
         REQUIRE(bench.module.resetDm() == Status::Ok);
      }
      SUBCASE("After a resume") {
         bench.connect();
         REQUIRE(bench.module.resume() == Status::Ok);
      }
      bench.take();
      CHECK(bench.module.resume() == Status::Ok);
      // resumereq cleared resumeack: the poll ends at allrunning.
      CHECK(bench.take() == std::vector{ W(dm::dmcontrol, 0x0000'0001), W(dm::dmcontrol, 0x4000'0001), R(dm::dmstatus, 0x0000'0c82) });
      CHECK_FALSE(bench.target.halted);
   }
}

TEST_CASE("A misaligned address gives Misaligned before any frame") {
   for(const Wire wire : wires) {
      CAPTURE(wire);
      Bench bench(wire);
      bench.connect();
      uint32_t value = 0;
      CHECK(bench.module.readMemory(0x2000'0002, value) == Status::Misaligned);
      CHECK(bench.module.writeMemory(0x2000'0001, 0) == Status::Misaligned);
      bench.module.setMemoryAccess(MemoryAccess::Abstract);
      CHECK(bench.module.readMemory(0x2000'0001, value, Width::Half) == Status::Misaligned);
      CHECK(bench.module.writeMemory(0x2000'0003, 0, Width::Half) == Status::Misaligned);
      CHECK(bench.take().empty());
      CHECK(bench.module.readMemory(0x2000'0003, value, Width::Byte) == Status::Ok);
      CHECK(bench.module.readMemory(0x2000'0002, value, Width::Half) == Status::Ok);
   }
}

TEST_CASE("Program buffer: a fault restores a0 and a1") {
   Bench bench(Wire::One);
   bench.connect();
   bench.target.gprs[10] = 0xaaaa'0010;
   bench.target.gprs[11] = 0xaaaa'0011;
   bench.target.faultAddresses.insert(0x2000'0000);
   bench.target.store(0x2000'0004, 0x1234'5678, 4);
   uint32_t value = 0;

   SUBCASE("Read") {
      CHECK(bench.module.readMemory(0x2000'0000, value) == Status::Command);
   }
   SUBCASE("Write") {
      CHECK(bench.module.writeMemory(0x2000'0000, 0x5555'5555) == Status::Command);
      CHECK(bench.target.memory.count(0x2000'0000) == 0);
   }
   CHECK(bench.module.cmderr() == rvswd::cmderrException);
   CHECK(bench.target.cmderr() == 0);
   CHECK(bench.target.gprs[10] == 0xaaaa'0010);
   CHECK(bench.target.gprs[11] == 0xaaaa'0011);
   const auto accesses = bench.take();
   // The restore program, then its run
   const std::vector<Access> tail(accesses.end() - 5, accesses.end());
   CHECK(tail == std::vector{ W(0x20, 0x7b20'2573), W(0x21, 0x7b30'25f3), W(0x22, 0x0000'9002),
                              W(dm::command, 0x0004'0000), R(dm::abstractcs, 0x0800'0002) });

   // The next access loads its program again.
   CHECK(bench.module.readMemory(0x2000'0004, value) == Status::Ok);
   CHECK(value == 0x1234'5678);
   CHECK(bench.take().front() == W(dm::abstractauto, 0));
   CHECK(bench.target.gprs[10] == 0xaaaa'0010);
   CHECK(bench.target.gprs[11] == 0xaaaa'0011);
}

TEST_CASE("Abstract memory access: a bus error gives Command") {
   Bench bench(Wire::Two);
   bench.connect();
   bench.target.faultAddresses.insert(0x2000'0000);
   uint32_t value = 0;
   CHECK(bench.module.readMemory(0x2000'0000, value) == Status::Command);
   CHECK(bench.module.cmderr() == 5);
   CHECK(bench.module.readMemory(0x2000'0004, value) == Status::Ok);
}

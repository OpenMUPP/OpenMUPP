#pragma once

// The CMSIS-DAP v2.1 command processor for SWD. Hardware-independent: it
// runs on a DapSwd object, a protocol::GpioPins object and a Platform
// object, and the host unit tests use mock ones. A USB function (a later
// change) moves the packets.
//
// The transport calls submit() for each request packet (at most
// packetSize bytes) and sends the response packets in order. Commands:
//
//   0x00 DAP_Info             0x10 DAP_SWJ_Pins        0x1D DAP_SWD_Sequence
//   0x01 DAP_HostStatus       0x11 DAP_SWJ_Clock       0x7E DAP_QueueCommands
//   0x02 DAP_Connect          0x12 DAP_SWJ_Sequence    0x7F DAP_ExecuteCommands
//   0x03 DAP_Disconnect       0x13 DAP_SWD_Configure
//   0x04 DAP_TransferConfigure
//   0x05 DAP_Transfer
//   0x06 DAP_TransferBlock
//   0x08 DAP_WriteABORT
//   0x09 DAP_Delay
//   0x0A DAP_ResetTarget
//
// The transfer commands follow the reference implementation (CMSIS-DAP
// DAP.c): posted AP reads, RDBUFF at the end of a read sequence, a read of
// RDBUFF after the last write, value match with a retry count, and the
// skip of the requests after an error.
//
// - A command that is unknown replies ID_DAP_Invalid (0xFF).
// - JTAG is not available yet: DAP_Connect to JTAG replies 0 (failed).
// - DAP_TransferAbort (0x07) has no response. Nothing runs concurrently,
//   so it has no effect yet.
// - A request that is shorter than its command needs, or a response that
//   does not fit in a packet, gives ID_DAP_Invalid. In a DAP_ExecuteCommands
//   it stops the list, and the command count is the number of commands
//   that ran.
// - DAP_Disconnect and hostGone() release the pins that DAP_SWJ_Pins
//   drove. nRESET and nTRST get the pull-up, as in the GPIO block.
// - A request is checked, with the room for its response, before the first
//   transfer. A bad request does not reach the target.
// - The timestamp bit of DAP_Transfer is ignored: the capabilities do not
//   include the test domain timer.
// - DAP_ExecuteCommands and DAP_QueueCommands do not nest.
//
// DAP_QueueCommands packets wait. The next packet with another command
// runs the queued packets and then itself, back to back. The responses of
// the queued packets have the ID of DAP_ExecuteCommands, as in the
// reference implementation.
//
// Each packet that has no response yet takes one slot of the packetCount
// slots. The transport must call canSubmit() before submit(), and send the
// responses (response(), popResponse()) to make room.

#include <array>
#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/GpioBlock.h"
#include "protocol/cmsisdap/DapSwd.h"

namespace cmsisdap {

namespace command {
inline constexpr uint8_t info = 0x00;
inline constexpr uint8_t hostStatus = 0x01;
inline constexpr uint8_t connect = 0x02;
inline constexpr uint8_t disconnect = 0x03;
inline constexpr uint8_t transferConfigure = 0x04;
inline constexpr uint8_t transfer = 0x05;
inline constexpr uint8_t transferBlock = 0x06;
inline constexpr uint8_t transferAbort = 0x07;
inline constexpr uint8_t writeAbort = 0x08;
inline constexpr uint8_t delay = 0x09;
inline constexpr uint8_t resetTarget = 0x0A;
inline constexpr uint8_t swjPins = 0x10;
inline constexpr uint8_t swjClock = 0x11;
inline constexpr uint8_t swjSequence = 0x12;
inline constexpr uint8_t swdConfigure = 0x13;
inline constexpr uint8_t swdSequence = 0x1D;
inline constexpr uint8_t queueCommands = 0x7E;
inline constexpr uint8_t executeCommands = 0x7F;
inline constexpr uint8_t invalid = 0xFF;
} // namespace command

// The IDs of DAP_Info
namespace infoId {
inline constexpr uint8_t vendor = 0x01;
inline constexpr uint8_t product = 0x02;
inline constexpr uint8_t serialNumber = 0x03;
inline constexpr uint8_t protocolVersion = 0x04;
inline constexpr uint8_t targetDeviceVendor = 0x05;
inline constexpr uint8_t targetDeviceName = 0x06;
inline constexpr uint8_t targetBoardVendor = 0x07;
inline constexpr uint8_t targetBoardName = 0x08;
inline constexpr uint8_t firmwareVersion = 0x09;
inline constexpr uint8_t capabilities = 0xF0;
inline constexpr uint8_t testDomainTimer = 0xF1;
inline constexpr uint8_t uartRxBufferSize = 0xFB;
inline constexpr uint8_t uartTxBufferSize = 0xFC;
inline constexpr uint8_t swoBufferSize = 0xFD;
inline constexpr uint8_t packetCount = 0xFE;
inline constexpr uint8_t packetSize = 0xFF;
} // namespace infoId

// The bits of the capabilities (DAP_Info 0xF0)
namespace capability {
inline constexpr uint16_t swd = 0x01;
inline constexpr uint16_t jtag = 0x02;
inline constexpr uint16_t atomicCommands = 0x10;
inline constexpr uint16_t uartPort = 0x80;
} // namespace capability

// The values of DAP_Info. A string that is nullptr, or too long for a
// packet, is empty or cut. The strings must stay valid.
struct DeviceInfo {
   const char *vendor{ nullptr };
   const char *product{ nullptr };
   // The chip UID, as text
   const char *serialNumber{ nullptr };
   const char *firmwareVersion{ nullptr };
   // JTAG and the UART port come with their commands.
   uint16_t capabilities{ capability::swd | capability::atomicCommands };
};

enum class Led : uint8_t {
   Connected,
   Running,
};

// Time and LEDs of the board.
class Platform {
   public:
   // A free-running microsecond counter. It wraps at 2^32.
   virtual uint32_t nowUs() = 0;
   virtual void delayUs(uint32_t microseconds) = 0;
   virtual void setLed(Led led, bool on) = 0;

   protected:
   ~Platform() = default;
};

class PacketReader;
class PacketWriter;

class Processor {
   public:
   // The size of a packet, and the packets in flight. DAP_Info reports
   // both.
   static constexpr std::size_t packetSize = 64;
   static constexpr std::size_t packetCount = 4;
   // The pulse of DAP_ResetTarget on SRST
   static constexpr uint32_t resetPulseUs = 10'000;
   // The SWCLK after a reset of the debug unit
   static constexpr uint32_t defaultClockHz = 1'000'000;
   // The longest wait of DAP_SWJ_Pins
   static constexpr uint32_t maxPinWaitUs = 3'000'000;

   Processor(DapSwd &swd, protocol::GpioPins &gpio, Platform &platform, const DeviceInfo &info);

   [[nodiscard]] bool canSubmit() const {
      return queued_ + responses_ < packetCount;
   }

   // Runs a request packet, or queues it (DAP_QueueCommands). Needs
   // canSubmit(). A packet of more than packetSize bytes gives ID_DAP_Invalid.
   void submit(art::span<const uint8_t> request);

   [[nodiscard]] bool hasResponse() const {
      return responses_ != 0;
   }
   // The oldest response packet, valid until popResponse().
   [[nodiscard]] art::span<const uint8_t> response() const;
   void popResponse();

   // The host went away: like DAP_Disconnect, and the queue is empty.
   void hostGone();

   [[nodiscard]] bool connected() const {
      return connected_;
   }

   private:
   using Packet = std::array<uint8_t, packetSize>;

   [[nodiscard]] bool processCommand(PacketReader &in, PacketWriter &out, bool nested);
   void execute(art::span<const uint8_t> request);
   void flushQueue();

   [[nodiscard]] bool infoCommand(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool hostStatus(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool connect(PacketReader &in, PacketWriter &out);
   void disconnect();
   [[nodiscard]] bool transferConfigure(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool transfer(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool transferBlock(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool writeAbort(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool delay(PacketReader &in, PacketWriter &out);
   void resetTarget(PacketWriter &out);
   [[nodiscard]] bool swjPins(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool swjClock(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool swjSequence(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool swdConfigure(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool swdSequence(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool executeCommands(uint8_t id, PacketReader &in, PacketWriter &out);

   [[nodiscard]] uint8_t pinsIn() const;

   DapSwd &swd_;
   protocol::GpioPins &gpio_;
   Platform &platform_;
   DeviceInfo info_;

   bool connected_{ false };
   SwdSettings settings_;
   uint16_t matchRetries_{ 0 };
   uint32_t matchMask_{ 0 };
   uint32_t clockHz_{ defaultClockHz };

   // The queued request packets, and the response packets
   std::array<Packet, packetCount> queue_{};
   std::array<uint8_t, packetCount> queueLength_{};
   std::size_t queued_{ 0 };
   std::array<Packet, packetCount> responseBuffer_{};
   std::array<uint8_t, packetCount> responseLength_{};
   std::size_t responseFirst_{ 0 };
   std::size_t responses_{ 0 };
};

} // namespace cmsisdap

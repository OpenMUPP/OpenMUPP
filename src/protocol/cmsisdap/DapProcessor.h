#pragma once

// The CMSIS-DAP v2.1 command processor for SWD and JTAG. Hardware-
// independent: it runs on a DapSwd object, a protocol::JtagPort object, a
// protocol::GpioPins object and a Platform object, and the host unit tests
// use mock ones. usb::CmsisDapFunction moves the packets.
//
// The transport calls submit() for each request packet (at most
// packetSize bytes). submit() only stores the packet. run() runs the
// stored packets, in the main loop. The transport sends the response
// packets in order. Commands:
//
//   0x00 DAP_Info               0x10 DAP_SWJ_Pins       0x7E DAP_QueueCommands
//   0x01 DAP_HostStatus         0x11 DAP_SWJ_Clock      0x7F DAP_ExecuteCommands
//   0x02 DAP_Connect            0x12 DAP_SWJ_Sequence
//   0x03 DAP_Disconnect         0x13 DAP_SWD_Configure
//   0x04 DAP_TransferConfigure  0x14 DAP_JTAG_Sequence
//   0x05 DAP_Transfer           0x15 DAP_JTAG_Configure
//   0x06 DAP_TransferBlock      0x16 DAP_JTAG_IDCODE
//   0x08 DAP_WriteABORT         0x1D DAP_SWD_Sequence
//   0x09 DAP_Delay
//   0x0A DAP_ResetTarget        0x80 ID_DAP_Vendor0
//
// The transfer commands follow the reference implementation (CMSIS-DAP
// DAP.c): posted AP reads, RDBUFF at the end of a read sequence, a read of
// RDBUFF after the last write, value match with a retry count, and the
// skip of the requests after an error.
//
// - A command that is unknown replies ID_DAP_Invalid (0xFF).
// - An error status is DAP_ERROR (0xFF), as in the specification. Only
//   DAP_Connect differs: it replies port 0 for a failure.
// - Without a JTAG port, DAP_Connect to JTAG replies 0 (failed), and
//   DAP_Info does not report JTAG. On JTAG, the transfer commands, and
//   DAP_WriteABORT, use the device index of the request and the chain of
//   DAP_JTAG_Configure (DapJtag.h). A device index that is not in the
//   chain gives no transfer.
// - DAP_SWJ_Clock sets SWCLK and TCK. TCK is in kHz, rounded down, and at
//   least 1 kHz.
// - DAP_TransferAbort (0x07) has no response and takes no slot. It stops
//   the transfer commands of the packets that came before it: the packet
//   that runs, and the packets that wait. A DAP_Transfer or
//   DAP_TransferBlock checks it before each transfer, and replies the
//   transfers that it did. On SWD, the engine also checks it after each
//   WAIT (DapSwd::setAbort()): the abort stops the WAIT retries, and the
//   response value is WAIT. A packet after the abort runs normally. When no
//   packet runs or waits, the abort has no effect.
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
// - ID_DAP_Vendor0 [0x80][personality u8] -> [0x80][status u8] switches
//   the USB personality of a dual image (personality/PersonalitySwitch.h).
//   The personality is the code of personality/Personality.h: 0 Versaloon,
//   1 CMSIS-DAP. The status is DAP_OK (0) or DAP_ERROR (0xFF) for a code
//   that is not a personality. Without a personality switch (an image with one
//   personality), the command replies ID_DAP_Invalid.
//
// DAP_QueueCommands packets wait. The next packet with another command
// runs the queued packets and then itself, back to back. The responses of
// the queued packets have the ID of DAP_ExecuteCommands, as in the
// reference implementation. A queued packet that takes the last free slot
// runs the queue at once: else no slot is free for the packet that ends
// the queue.
//
// Each packet that has no response yet takes one slot of the packetCount
// slots: a stored packet, a queued packet and the packet that runs. The
// transport must call canSubmit() before submit(), and send the responses
// (response(), popResponse()) to make room.
//
// submit(), response() and popResponse() can also run during run(), from
// the background service of the SWD engine (protocol/Background.h). A
// packet that arrives then waits until the running packet ends. runNext()
// does not start again from there.

#include <array>
#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/GpioBlock.h"
#include "protocol/JtagPort.h"
#include "protocol/cmsisdap/DapJtag.h"
#include "protocol/cmsisdap/DapSwd.h"

namespace personality {
class PersonalitySwitch;
}

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
inline constexpr uint8_t jtagSequence = 0x14;
inline constexpr uint8_t jtagConfigure = 0x15;
inline constexpr uint8_t jtagIdcode = 0x16;
inline constexpr uint8_t swdSequence = 0x1D;
inline constexpr uint8_t vendor0 = 0x80;
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
   // The processor adds JTAG when it has a JTAG port. The UART port comes
   // with its commands.
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

class Processor final : private TransferAbort {
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

   // jtag: nullptr for a board without JTAG. personalitySwitch: nullptr
   // for an image with one personality.
   Processor(DapSwd &swd,
             protocol::GpioPins &gpio,
             Platform &platform,
             const DeviceInfo &info,
             protocol::JtagPort *jtag = nullptr,
             personality::PersonalitySwitch *personalitySwitch = nullptr);

   [[nodiscard]] bool canSubmit() const {
      return requests_ + responses_ < packetCount;
   }

   // Stores a request packet for run(). Needs canSubmit(). A packet of more
   // than packetSize bytes gives ID_DAP_Invalid.
   void submit(art::span<const uint8_t> request);

   // Runs the next stored packet. Returns false when no packet can run:
   // none is stored, or only DAP_QueueCommands packets wait. Call it in the
   // main loop, not during runNext().
   bool runNext();

   // Runs the stored packets until runNext() returns false.
   void run() {
      while(runNext()) {
      }
   }

   [[nodiscard]] bool hasResponse() const {
      return responses_ != 0;
   }
   // The oldest response packet, valid until popResponse().
   [[nodiscard]] art::span<const uint8_t> response() const;
   void popResponse();

   // The host went away: like DAP_Disconnect, and the queue is empty.
   void hostGone();

   // Like DAP_Disconnect: releases the pins. The queued packets and the
   // responses stay. For a suspend, where the transport keeps its state.
   void release() {
      disconnect();
   }

   [[nodiscard]] bool connected() const {
      return port_ != Port::None;
   }

   private:
   enum class Port : uint8_t { None, Swd, Jtag };

   using Packet = std::array<uint8_t, packetSize>;
   // One byte more than a packet: a longer request gives ID_DAP_Invalid.
   using Request = std::array<uint8_t, packetSize + 1>;

   [[nodiscard]] bool processCommand(PacketReader &in, PacketWriter &out, bool nested);
   void execute(art::span<const uint8_t> request);
   [[nodiscard]] art::span<uint8_t> request(std::size_t index);
   [[nodiscard]] bool isQueued(std::size_t index);
   [[nodiscard]] bool mustWait();
   // DAP_TransferAbort stops the packet that runs. Also asked by the SWD
   // engine after a WAIT (DapSwd::setAbort()).
   [[nodiscard]] bool aborted() const override {
      return abortRequests_ != 0;
   }

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
   [[nodiscard]] bool jtagSequence(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool jtagConfigure(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool jtagIdcode(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool executeCommands(uint8_t id, PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool switchPersonality(PacketReader &in, PacketWriter &out);

   [[nodiscard]] uint8_t pinsIn() const;
   void applySettings();
   void setJtagClock();
   // Selects the device of a transfer command: true when the port can do
   // the transfers.
   [[nodiscard]] bool selectDevice(uint8_t index);
   // One transfer on the connected port, as DapSwd::transfer()
   [[nodiscard]] uint8_t dpTransfer(uint8_t request, uint32_t &data);

   DapSwd &swd_;
   JtagDp jtag_;
   protocol::GpioPins &gpio_;
   Platform &platform_;
   DeviceInfo info_;
   personality::PersonalitySwitch *personalitySwitch_;

   Port port_{ Port::None };
   // Also the idle cycles and the WAIT retries of JTAG
   SwdSettings settings_;
   uint16_t matchRetries_{ 0 };
   uint32_t matchMask_{ 0 };
   uint32_t clockHz_{ defaultClockHz };

   // The request packets in order: the packet that runs, the
   // DAP_QueueCommands packets that wait, and the packets that submit()
   // stored. And the response packets.
   std::array<Request, packetCount> requestBuffer_{};
   std::array<uint8_t, packetCount> requestLength_{};
   std::size_t requestFirst_{ 0 };
   std::size_t requests_{ 0 };
   bool running_{ false };
   // The requests at the front that DAP_TransferAbort stops
   std::size_t abortRequests_{ 0 };
   std::array<Packet, packetCount> responseBuffer_{};
   std::array<uint8_t, packetCount> responseLength_{};
   std::size_t responseFirst_{ 0 };
   std::size_t responses_{ 0 };
};

} // namespace cmsisdap

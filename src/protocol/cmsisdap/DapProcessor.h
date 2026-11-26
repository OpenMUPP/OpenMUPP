// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The CMSIS-DAP v2.1 command processor for SWD and JTAG. Hardware-
// independent: it runs on a DapSwd object, a protocol::JtagPort object, a
// protocol::GpioPins object, a DapUart object, a DapSwo object and a
// Platform object, and
// the host unit tests use mock ones. usb::CmsisDapFunction moves the packets.
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
//   0x09 DAP_Delay              0x1F DAP_UART_Transport
//   0x0A DAP_ResetTarget        0x20 DAP_UART_Configure
//                               0x21 DAP_UART_Transfer
//   0x17 DAP_SWO_Transport      0x22 DAP_UART_Control
//   0x18 DAP_SWO_Mode           0x23 DAP_UART_Status
//   0x19 DAP_SWO_Baudrate
//   0x1A DAP_SWO_Control        0x80 ID_DAP_Vendor0
//   0x1B DAP_SWO_Status         0x81 ID_DAP_Vendor1
//   0x1C DAP_SWO_Data           0x82 ID_DAP_Vendor2
//                               0x83 ID_DAP_Vendor3
//                               0x84 ID_DAP_Vendor4
//                               0x85 ID_DAP_Vendor5
//                               0x86 ID_DAP_Vendor6
//   0x1E DAP_SWO_ExtendedStatus
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
//   that is not a personality. The code boot::bootloaderCode (0xB0) starts
//   the bootloader (boot/BootloaderEntry.h): DAP_ERROR in a multi image
//   without a bootloader. Without a personality switch (an image with one
//   personality) and without a bootloader, the command replies
//   ID_DAP_Invalid.
// - ID_DAP_Vendor1 [0x81][mode u8] -> [0x81][status u8] sets the mode of
//   the RISC-V bridge (RvBridge.h): 0 off, 1 on, 2 auto, 3 the stored
//   mode (bridgeStoredMode). On, DAP_Connect to
//   JTAG and the JTAG commands use the emulated TAP, and the TAP state and
//   IR are not known. Auto finds the port at each DAP_Connect to JTAG. With
//   bit 7 (bridgeStoreFlag) set, the command also stores the mode: it is
//   then the mode at power-up and after hostGone(). The status is DAP_OK
//   (0), or DAP_ERROR (0xFF) for another value and for a failed store. A
//   failed store still sets the mode. Without a bridge, the command replies
//   ID_DAP_Invalid. hostGone() sets the stored mode.
//   DAP_SWJ_Pins with SWCLK or SWDIO selected tells the bridge that the
//   pins changed (Bridge::pinsChanged()).
// - ID_DAP_Vendor2 [0x82][op u8] -> [0x82][status u8][mV u16][state u8]
//   switches the target supply (protocol/TargetPower.h): op 0 off, 1 on at
//   3.3 V, 2 only read, and [0x82][3][mV u16] on at 3300 or 5000 mV. The
//   status is 0 (done), 1 (a protection refused the switch-on), or 0xFF:
//   a switch that the board does not have, another voltage, or another op.
//   mV is the target voltage after the operation, 0 without a sense or
//   when the measurement fails. state bits 0-1 are the supply (0 off,
//   1 3.3 V, 2 5 V). Bit 7 is set once after a fault switched the supply
//   off. Without a TargetPower, the command replies
//   ID_DAP_Invalid. hostGone() keeps the supply on. With a pin owner, op 1
//   and op 3 reply status 1 while the other processor owns the pins: only the
//   owner, or any processor while nobody owns the pins, switches the
//   supply on. Op 0 always works.
// - ID_DAP_Vendor3 [0x83][op u8] -> [0x83][status u8][setting u8] reads
//   and writes the setting "VPP without an adapter" (VppSetting,
//   setVppSetting()): op 0 stores off, 1 stores on, 2 only reads. The
//   status is DAP_OK (0), or DAP_ERROR (0xFF) for another op and for a
//   failed store. setting is the stored value after the operation: 0 off,
//   1 on. Without a VppSetting, the command replies ID_DAP_Invalid.
// - ID_DAP_Vendor4 [0x84] -> [0x84][status u8][board kind u8][board slot
//   u8][adapter kind u8][adapter slot u8][allowed u8] reads the board ID
//   and the adapter ID (IdSource, setIdSource()). The adapter ID is read
//   now. The command is not a connect: the target gate keeps its decision.
//   kind: 0 no ID resistor (open), 1 a known slot, 2 a fault (short), 3 a
//   free slot. For the board ID, kind 2 is also an ADC failure at
//   start-up. slot is 1 to 9 for kinds 1 and 3, else 0. allowed is 1 when
//   a connect would start the protocol now. The status is DAP_OK (0), or
//   DAP_ERROR (0xFF) when the ADC fails on the adapter ID: the adapter
//   fields and allowed are then 0. Without an IdSource, the command
//   replies ID_DAP_Invalid.
// - ID_DAP_Vendor5 [0x85][op u8] -> [0x85][status u8][level mV u16][sense mV
//   u16][sense2 mV u16] controls VPP for the hardware test (VppCommand,
//   setVpp(); DapVpp.h has the fields). It takes the rest of the packet,
//   so in a DAP_ExecuteCommands it replies ID_DAP_Invalid. With a pin
//   owner, a switch-on is refused while the other processor owns the
//   pins. Without a VppCommand, the command replies ID_DAP_Invalid.
//   hostGone() and release() switch VPP off (VppCommand::hostGone()).
//   The VPP code is not in the processor: the images without VPP do not
//   link it.
// - ID_DAP_Vendor6 [0x86][op u8] -> [0x86][status u8][mode u8] sets the
//   wire mode of the UART (UartWire, setUartWire()): op 0 normal (two
//   wires), 1 single wire, 2 only read. The status is DAP_OK (0), or
//   DAP_ERROR (0xFF) for another op. mode is the mode after the operation:
//   0 normal, 1 single wire. hostGone() keeps the mode. Without a UartWire,
//   the command replies ID_DAP_Invalid.
// - With vendor commands (setVendorCommands()), 0x80-0x82 go to that
//   object instead (the EDBG commands AVR_CMD, AVR_RSP, AVR_EVT on the HID
//   interface). They take the rest of the packet, so in a
//   DAP_ExecuteCommands they reply ID_DAP_Invalid. When the processor
//   gives the pins back (DAP_Disconnect, release(), hostGone()) while it
//   owns them, it calls VendorCommands::releasePins() first. hostGone()
//   then calls VendorCommands::hostGone().
// - With a pin owner (sharePins()), the processor shares the engines and
//   the pins with another processor (protocol/TargetPinOwner.h). DAP_Connect,
//   DAP_SWJ_Pins and DAP_ResetTarget take the pins. When the other
//   processor owns them, DAP_Connect replies port 0, DAP_SWJ_Pins drives no
//   pin and only reads them, and DAP_ResetTarget replies DAP_ERROR.
//   DAP_Disconnect, release() and hostGone() give the pins back, and touch
//   no pin while the other processor owns them. DAP_SWJ_Clock,
//   DAP_TransferConfigure and DAP_SWD_Configure store their values and
//   change the engines only while the processor owns the pins. DAP_Connect
//   applies them. ID_DAP_Vendor1 replies DAP_ERROR, and hostGone() keeps
//   the bridge mode, while the other processor owns the pins: the bridge
//   is its JTAG port too.
// - With a target gate (setTargetGate(), protocol/TargetGate.h), DAP_Connect
//   asks the gate first (TargetGate::check()). When the gate refuses,
//   DAP_Connect ends the old port and replies port 0. DAP_SWJ_Pins and
//   DAP_ResetTarget then work as when the other processor owns the pins
//   (see below), until a DAP_Connect that the gate allows.
// - errorCount() counts the transfers of DAP_Transfer and DAP_TransferBlock
//   that end with an error on the wire: FAULT, no acknowledge, a parity
//   error, or WAIT after all retries. A WAIT that DAP_TransferAbort stops
//   and a value mismatch are not errors of the wire. On JTAG, OK and FAULT
//   are one acknowledge, and the processor does not read STICKYERR: only
//   WAIT after all retries, an invalid acknowledge and a failed scan count.
//   The error LED shows the count (ErrorLed.h).
// - The DAP_UART_* commands run on the DapUart object (DapUart.h). Without
//   one, they reply ID_DAP_Invalid, and DAP_Info reports no UART. With one,
//   DAP_Info reports the UART port, the USB COM port when the UART has it,
//   and the buffer sizes. A DAP_UART_Transfer takes the received bytes
//   that fit in the response, and replies the status after the transfer.
//   A Tx Count that is more than the request has gives ID_DAP_Invalid.
// - The DAP_SWO_* commands run on the DapSwo object (DapSwo.h, setSwo()).
//   Without one, they reply ID_DAP_Invalid, and DAP_Info reports no SWO.
//   With one, DAP_Info reports the SWO modes of the receiver (UART,
//   Manchester) and the buffer size, and SWO streaming trace when the
//   transport has the SWO trace endpoint. The processor keeps the
//   transport, the mode and the baud rate:
//   - DAP_SWO_Transport accepts 0 (none), 1 (DAP_SWO_Data), and 2 (the
//     SWO trace endpoint) only with streaming. While the capture runs, it
//     gives DAP_ERROR. With transport 2, the transport takes the data with
//     readSwoStream().
//   - DAP_SWO_Mode stops the capture, and clears the baud rate. It accepts
//     0 (off) and the modes of the receiver (1 UART, 2 Manchester).
//     Another mode gives DAP_ERROR and sets mode off.
//   - DAP_SWO_Baudrate replies the real baud rate of the receiver, and
//     stores it. A baud rate above the maximum gives the maximum. While the
//     capture runs, the new baud rate applies at once and the capture goes
//     on. In mode off, or for a baud rate below the minimum, it replies 0,
//     stores 0 and stops the capture.
//   - DAP_SWO_Control bit 0 starts or stops the capture. A start needs a
//     mode, a baud rate and no JTAG port, else it gives DAP_ERROR. A start empties the
//     buffer and clears the errors. A start while the capture runs, or a
//     stop while it does not run, does nothing.
//   - DAP_Connect to JTAG stops the capture: SWO is for SWD, and on the
//     CH32V307 the Manchester capture shares a DMA channel with the JTAG
//     fast path (swo/SwoReceiver.h).
//   - The trace status has the bits active (0x01), error (0x40: a framing
//     or noise error) and overrun (0x80: data lost). The receiver does not
//     pause on a full buffer, so the paused bit (0x02) is never set. Each
//     read of the status clears the error bits: DAP_SWO_Status,
//     DAP_SWO_Data, and DAP_SWO_ExtendedStatus with bit 0. A read of only
//     the count clears nothing.
//   - DAP_SWO_ExtendedStatus gives the status (bit 0) and the count
//     (bit 1). Bit 2 (index and timestamp) is ignored: the capabilities do
//     not include the test domain timer.
//   - DAP_SWO_Data gives up to the requested count, and at most what fits
//     in the response (packet size - 4). With a transport other than 1, it
//     gives no data. The status is read before the data, as in the
//     reference implementation.
//   The commands follow the reference implementation (CMSIS-DAP SWO.c).
//   - DAP_Disconnect does not stop the capture. hostGone() stops it, and
//     sets transport none, mode off and baud rate 0.
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

#include "protocol/GpioPins.h"
#include "protocol/JtagPort.h"
#include "protocol/TargetGate.h"
#include "protocol/TargetPinOwner.h"
#include "protocol/TargetPower.h"
#include "protocol/cmsisdap/DapJtag.h"
#include "protocol/cmsisdap/DapSwd.h"
#include "protocol/cmsisdap/DapSwo.h"
#include "protocol/cmsisdap/DapUart.h"

namespace boot {
class BootloaderEntry;
}

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
inline constexpr uint8_t swoTransport = 0x17;
inline constexpr uint8_t swoMode = 0x18;
inline constexpr uint8_t swoBaudrate = 0x19;
inline constexpr uint8_t swoControl = 0x1A;
inline constexpr uint8_t swoStatus = 0x1B;
inline constexpr uint8_t swoData = 0x1C;
inline constexpr uint8_t swoExtendedStatus = 0x1E;
inline constexpr uint8_t swdSequence = 0x1D;
inline constexpr uint8_t uartTransport = 0x1F;
inline constexpr uint8_t uartConfigure = 0x20;
inline constexpr uint8_t uartTransfer = 0x21;
inline constexpr uint8_t uartControl = 0x22;
inline constexpr uint8_t uartStatus = 0x23;
inline constexpr uint8_t vendor0 = 0x80;
inline constexpr uint8_t vendor1 = 0x81;
inline constexpr uint8_t vendor2 = 0x82;
inline constexpr uint8_t vendor3 = 0x83;
inline constexpr uint8_t vendor4 = 0x84;
inline constexpr uint8_t vendor5 = 0x85;
inline constexpr uint8_t vendor6 = 0x86;
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
inline constexpr uint16_t swoUart = 0x04;
inline constexpr uint16_t swoManchester = 0x08;
inline constexpr uint16_t swoStreaming = 0x40;
inline constexpr uint16_t atomicCommands = 0x10;
inline constexpr uint16_t uartPort = 0x80;
inline constexpr uint16_t usbComPort = 0x100;
} // namespace capability

// The values of DAP_Info. A string that is nullptr, or too long for a
// packet, is empty or cut. The strings must stay valid.
struct DeviceInfo {
   const char *vendor{ nullptr };
   const char *product{ nullptr };
   // The chip UID, as text
   const char *serialNumber{ nullptr };
   const char *firmwareVersion{ nullptr };
   // The processor adds JTAG when it has a JTAG port, the UART port and
   // the USB COM port when it has a DapUart, and the SWO modes (and SWO
   // streaming trace) when it has a DapSwo.
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

// The modes of the RISC-V bridge: the values of ID_DAP_Vendor1
enum class BridgeMode : uint8_t {
   Off = 0,
   On = 1,
   Auto = 2,
};

// ID_DAP_Vendor1: this bit of the mode byte also stores the mode
inline constexpr uint8_t bridgeStoreFlag = 0x80;
// ID_DAP_Vendor1: this mode value sets the stored mode. It takes no store flag.
inline constexpr uint8_t bridgeStoredMode = 3;

// The RISC-V bridge of ID_DAP_Vendor1 (RvBridge.h)
class Bridge {
   public:
   virtual void setMode(BridgeMode mode) = 0;
   // Stores the mode of the power-up and of hostGone(). False when the
   // store failed: the stored mode then stays.
   virtual bool storeMode(BridgeMode mode) = 0;
   [[nodiscard]] virtual BridgeMode storedMode() const = 0;
   // DAP_SWJ_Pins drove SWCLK or SWDIO.
   virtual void pinsChanged() = 0;

   protected:
   ~Bridge() = default;
};

// The stored setting "VPP without an adapter" of ID_DAP_Vendor3 (board/IdSense.h)
class VppSetting {
   public:
   // The stored value: false when none is stored
   [[nodiscard]] virtual bool load() const = 0;
   // False when the store failed
   virtual bool store(bool allowed) = 0;

   protected:
   ~VppSetting() = default;
};

// The board ID and the adapter ID of ID_DAP_Vendor4. The kind codes are
// the ones of the command.
struct IdReport {
   uint8_t boardKind;
   uint8_t boardSlot;
   uint8_t adapterKind;
   uint8_t adapterSlot;
   bool allowed;
   // False when the ADC fails on the adapter ID
   bool adapterRead;
};

class IdSource {
   public:
   [[nodiscard]] virtual IdReport read() = 0;

   protected:
   ~IdSource() = default;
};

// ID_DAP_Vendor5 of one processor (setVpp(), DapVpp.h)
class VppCommand {
   public:
   // Runs the command with the bytes after the ID (the rest of the
   // packet). Writes the response, with the ID, and returns its length:
   // at most response.size(), 0 for ID_DAP_Invalid. pinsFree: false while
   // the other processor owns the target pins.
   virtual std::size_t command(art::span<const uint8_t> fields, art::span<uint8_t> response, bool pinsFree) = 0;
   // The host went away or suspended the device: switch VPP off.
   virtual void hostGone() = 0;

   protected:
   ~VppCommand() = default;
};

// The wire mode of the UART of ID_DAP_Vendor6 (uart/SingleWireLine.h)
class UartWire {
   public:
   virtual void setSingleWire(bool on) = 0;
   [[nodiscard]] virtual bool singleWire() const = 0;

   protected:
   ~UartWire() = default;
};

// The vendor commands 0x80-0x82 of one processor (setVendorCommands())
class VendorCommands {
   public:
   // Runs the command id with the bytes after the ID (the rest of the
   // packet). Writes the response, with the ID, and returns its length:
   // at most response.size(), 0 for ID_DAP_Invalid.
   virtual std::size_t command(uint8_t id, art::span<const uint8_t> fields, art::span<uint8_t> response) = 0;
   // The processor gives the target pins back: stop driving them.
   virtual void releasePins() = 0;
   // The host went away: drop the state of the old host.
   virtual void hostGone() = 0;

   protected:
   ~VendorCommands() = default;
};

class PacketReader;
class PacketWriter;

class Processor final : private TransferAbort {
   public:
   // The packet size of a full-speed USB bulk endpoint, the size after
   // construction.
   static constexpr std::size_t fullSpeedPacketSize = 64;
   // The largest packet size: the buffers have this size. 512 (a
   // high-speed bulk endpoint) on the CH32V307 and in the host tests; 64
   // on the STM32 boards, which have full-speed USB only and less RAM.
#if defined(__arm__) && !defined(UNIT_TESTING)
   static constexpr std::size_t maxPacketSize = fullSpeedPacketSize;
#else
   static constexpr std::size_t maxPacketSize = 512;
#endif
   // The packets in flight. DAP_Info reports it and packetSize().
   static constexpr std::size_t packetCount = 4;
   // The pulse of DAP_ResetTarget on SRST
   static constexpr uint32_t resetPulseUs = 10'000;
   // The SWCLK after a reset of the debug unit
   static constexpr uint32_t defaultClockHz = 1'000'000;
   // The longest wait of DAP_SWJ_Pins
   static constexpr uint32_t maxPinWaitUs = 3'000'000;

   // jtag: nullptr for a board without JTAG. personalitySwitch: nullptr
   // for an image with one personality. uart: nullptr for no DAP_UART_*
   // commands. The constructor only stores the uart pointer, so the object
   // can be constructed later. bootloader: nullptr for an image without a
   // bootloader. bridge: nullptr for an image without the RISC-V bridge;
   // else the bridge is also the jtag port. power: nullptr for no
   // ID_DAP_Vendor2.
   Processor(DapSwd &swd,
             protocol::GpioPins &gpio,
             Platform &platform,
             const DeviceInfo &info,
             protocol::JtagPort *jtag = nullptr,
             personality::PersonalitySwitch *personalitySwitch = nullptr,
             DapUart *uart = nullptr,
             boot::BootloaderEntry *bootloader = nullptr,
             Bridge *bridge = nullptr,
             protocol::TargetPower *power = nullptr);

   [[nodiscard]] bool canSubmit() const {
      return requests_ + responses_ < packetCount;
   }

   // The packet size of the transport: fullSpeedPacketSize up to
   // maxPacketSize, else it is clamped. Set it while no packet is in flight
   // (after hostGone()): the stored packets keep their length.
   void setPacketSize(std::size_t size);
   [[nodiscard]] std::size_t packetSize() const {
      return packetSize_;
   }

   // Stores a request packet for run(). Needs canSubmit(). A packet of more
   // than packetSize() bytes gives ID_DAP_Invalid.
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

   // The host went away: like DAP_Disconnect, and the queue is empty. The
   // UART goes back to its default transport.
   void hostGone();

   // Like DAP_Disconnect: releases the pins and switches VPP off. The
   // queued packets and the responses stay. For a suspend, where the
   // transport keeps its state.
   void release();

   [[nodiscard]] bool connected() const {
      return port_ != Port::None;
   }

   // Shares the engines and the target pins with another processor:
   // `client` is the interface of this processor. The owner must outlive
   // the processor. Call it before the first packet.
   void sharePins(protocol::TargetPinOwner &owner, protocol::PinClient client) {
      pinOwner_ = &owner;
      pinClient_ = client;
   }

   // Asks `gate` at each DAP_Connect. The gate must outlive the processor.
   // Call it before the first packet.
   void setTargetGate(protocol::TargetGate &gate) {
      gate_ = &gate;
   }

   // Gives ID_DAP_Vendor3 to `setting`, which must outlive the processor.
   // Call it before the first packet.
   void setVppSetting(VppSetting &setting) {
      vppSetting_ = &setting;
   }

   // Gives ID_DAP_Vendor4 to `source`, which must outlive the processor.
   // Call it before the first packet.
   void setIdSource(IdSource &source) {
      idSource_ = &source;
   }

   // Gives ID_DAP_Vendor5 to `vpp`, which must outlive the processor.
   // Call it before the first packet.
   void setVpp(VppCommand &vpp) {
      vpp_ = &vpp;
   }

   // Gives ID_DAP_Vendor6 to `wire`, which must outlive the processor.
   // Call it before the first packet.
   void setUartWire(UartWire &wire) {
      uartWire_ = &wire;
   }

   // Sends the commands 0x80-0x82 to `vendor`, which must outlive the
   // processor. Call it before the first packet.
   void setVendorCommands(VendorCommands &vendor) {
      vendor_ = &vendor;
   }

   // Gives the DAP_SWO_* commands to `swo`, which must outlive the
   // processor. streaming: the transport has the SWO trace endpoint and
   // calls readSwoStream(). Call it before the first packet.
   void setSwo(DapSwo &swo, bool streaming = false) {
      swo_ = &swo;
      swoStreaming_ = streaming;
   }

   // The trace data for the SWO trace endpoint: up to data.size() bytes,
   // only with DAP_SWO_Transport 2. Returns the count.
   [[nodiscard]] std::size_t readSwoStream(art::span<uint8_t> data) {
      return swoTransport_ == swoTransport::streaming ? swo_->read(data) : 0;
   }

   // Number of transfers with an error on the wire. It wraps at 2^32: only
   // the difference between two values counts.
   [[nodiscard]] uint32_t errorCount() const {
      return errorCount_;
   }

   private:
   enum class Port : uint8_t { None, Swd, Jtag };

   using Packet = std::array<uint8_t, maxPacketSize>;
   // One byte more than a packet: a longer request gives ID_DAP_Invalid.
   using Request = std::array<uint8_t, maxPacketSize + 1>;

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
   void activatePort();
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
   [[nodiscard]] bool vendorCommand(uint8_t id, PacketReader &in, PacketWriter &out, bool nested);
   [[nodiscard]] bool switchPersonality(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool switchBridge(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool targetPower(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool vppSettingCommand(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool idCommand(PacketWriter &out);
   [[nodiscard]] bool uartWireCommand(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool uartCommand(uint8_t id, PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool uartTransfer(PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool swoCommand(uint8_t id, PacketReader &in, PacketWriter &out);
   [[nodiscard]] bool swoData(PacketReader &in, PacketWriter &out);
   // The trace status byte (swoStatus bits). Clears the error bits.
   [[nodiscard]] uint8_t swoTraceStatus();
   void swoReset();

   [[nodiscard]] uint8_t pinsIn() const;
   // Takes the target pins: false when the other processor owns them.
   [[nodiscard]] bool claimPins();
   // True when the processor may change the engines: it owns the pins, or
   // it has no pin owner.
   [[nodiscard]] bool ownsPins() const;
   // True when the other processor owns the pins.
   [[nodiscard]] bool otherOwnsPins() const;
   void applySettings();
   void setJtagClock();
   // Selects the device of a transfer command: true when the port can do
   // the transfers.
   [[nodiscard]] bool selectDevice(uint8_t index);
   // One transfer on the connected port, as DapSwd::transfer(). Counts
   // the errors of the wire.
   [[nodiscard]] uint8_t dpTransfer(uint8_t request, uint32_t &data);

   DapSwd &swd_;
   JtagDp jtag_;
   protocol::GpioPins &gpio_;
   Platform &platform_;
   DeviceInfo info_;
   personality::PersonalitySwitch *personalitySwitch_;
   DapUart *uart_;
   boot::BootloaderEntry *bootloader_;
   Bridge *bridge_;
   protocol::TargetPower *power_;
   VendorCommands *vendor_{ nullptr };
   protocol::TargetGate *gate_{ nullptr };
   VppSetting *vppSetting_{ nullptr };
   IdSource *idSource_{ nullptr };
   VppCommand *vpp_{ nullptr };
   UartWire *uartWire_{ nullptr };
   DapSwo *swo_{ nullptr };
   bool swoStreaming_{ false };
   protocol::TargetPinOwner *pinOwner_{ nullptr };
   protocol::PinClient pinClient_{ protocol::PinClient::None };

   Port port_{ Port::None };
   // True after the first command that drove the pins of port_
   bool portActive_{ false };
   // Also the idle cycles and the WAIT retries of JTAG
   SwdSettings settings_;
   uint16_t matchRetries_{ 0 };
   uint32_t matchMask_{ 0 };
   uint32_t clockHz_{ defaultClockHz };
   uint32_t errorCount_{ 0 };
   std::size_t packetSize_{ fullSpeedPacketSize };
   uint8_t swoTransport_{ swoTransport::none };
   uint8_t swoMode_{ swoMode::off };
   uint32_t swoBaud_{ 0 };

   // The request packets in order: the packet that runs, the
   // DAP_QueueCommands packets that wait, and the packets that submit()
   // stored. And the response packets.
   std::array<Request, packetCount> requestBuffer_{};
   std::array<uint16_t, packetCount> requestLength_{};
   std::size_t requestFirst_{ 0 };
   std::size_t requests_{ 0 };
   bool running_{ false };
   // The requests at the front that DAP_TransferAbort stops
   std::size_t abortRequests_{ 0 };
   std::array<Packet, packetCount> responseBuffer_{};
   std::array<uint16_t, packetCount> responseLength_{};
   std::size_t responseFirst_{ 0 };
   std::size_t responses_{ 0 };
};

} // namespace cmsisdap

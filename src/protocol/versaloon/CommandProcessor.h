#pragma once

// Runs one Versaloon command (protocol/versaloon/Versaloon.h) and writes its reply.
// Hardware-independent: the host unit tests check it.
//
// Common commands and their replies (all values little-endian):
//
//   GET_INFO      [0x00]        -> [buffer size u16][version text, NUL]
//   GET_TVCC      [0x01]        -> [target voltage in mV u16], 0 when
//                                  the handler does not know it
//   GET_HARDWARE  [0x02]        -> [hardware ID u8]
//   SWITCH_PERSONALITY [0x0E][personality u8]
//                               -> [status u8] (see
//                                  personality/PersonalitySwitch.h)
//   FW_UPDATE     [0x0F][0xAA]  -> [status u8] (see boot/BootloaderEntry.h)
//
// SWITCH_PERSONALITY takes the code of personality/Personality.h:
// 0 Versaloon, 1 CMSIS-DAP. Status 0 accepts the switch. Status 1 refuses
// it: an image without the switch (only one personality), or a code that
// is not a personality.
//
// The USB_TO_XXX commands (0x20-0x7F) go to a UsbToXxxHandler. The other
// commands have no reply.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utils/compat/span.h>

namespace boot {
class BootloaderEntry;
}

namespace personality {
class PersonalitySwitch;
}

namespace protocol {

// The identity of the firmware in GET_INFO and GET_HARDWARE
struct DeviceInfo {
   // Firmware version, for example "v0.1.0"
   std::string_view version;
   // Board name, for example "mini"
   std::string_view board;
   // Reply of GET_HARDWARE (board::Definition::hardwareId)
   uint8_t hardwareId;
};

// Runs the USB_TO_XXX commands (0x20-0x7F).
class UsbToXxxHandler {
   public:
   // command: the complete command, header included. reply: the reply
   // buffer. Returns the size of the reply, or no value for no reply.
   virtual std::optional<std::size_t> process(art::span<const uint8_t> command, art::span<uint8_t> reply) = 0;

   // The host closed the interface (bus reset, deconfigure or suspend),
   // for example because OpenOCD stopped. Release the target.
   virtual void hostGone() {
   }

   // The target voltage in mV for GET_TVCC. 0 when it is not known.
   [[nodiscard]] virtual uint16_t targetVoltage() {
      return 0;
   }

   protected:
   ~UsbToXxxHandler() = default;
};

class CommandProcessor {
   public:
   // The longest version text of GET_INFO, without the NUL. The host reads
   // the GET_INFO reply into a 256-byte buffer and adds a NUL after it.
   static constexpr std::size_t maxInfoText = 200;

   // bootloader: the bootloader entry of FW_UPDATE, nullptr when the
   // firmware has no bootloader (it then refuses FW_UPDATE). usbToXxx:
   // nullptr when there is no handler (the commands then have no reply).
   // personalitySwitch: nullptr when the image has one personality (it
   // then refuses SWITCH_PERSONALITY).
   CommandProcessor(const DeviceInfo &info,
                    boot::BootloaderEntry *bootloader,
                    UsbToXxxHandler *usbToXxx,
                    personality::PersonalitySwitch *personalitySwitch = nullptr);

   // command: a complete command (protocol/versaloon/CommandAssembler.h). reply: the
   // reply buffer, at least maxInfoText + 3 bytes. Returns the size of the
   // reply, or no value for no reply.
   std::optional<std::size_t> process(art::span<const uint8_t> command, art::span<uint8_t> reply);

   // See UsbToXxxHandler::hostGone().
   void hostGone();

   private:
   std::size_t info(art::span<uint8_t> reply) const;
   std::size_t firmwareUpdate(art::span<const uint8_t> command, art::span<uint8_t> reply);
   std::size_t personalitySwitch(art::span<const uint8_t> command, art::span<uint8_t> reply);

   DeviceInfo info_;
   boot::BootloaderEntry *bootloader_;
   UsbToXxxHandler *usbToXxx_;
   personality::PersonalitySwitch *personalitySwitch_;
};

} // namespace protocol

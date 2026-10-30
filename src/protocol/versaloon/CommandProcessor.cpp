#include "protocol/versaloon/CommandProcessor.h"

#include <algorithm>

#include "boot/BootloaderEntry.h"
#include "personality/PersonalitySwitch.h"
#include "protocol/versaloon/Versaloon.h"

namespace protocol {

namespace {

void putU16(art::span<uint8_t> reply, uint16_t value) {
   reply[0] = static_cast<uint8_t>(value);
   reply[1] = static_cast<uint8_t>(value >> 8);
}

// Copies text to out, as far as it fits in limit. Returns the new end.
uint8_t *append(uint8_t *out, const uint8_t *limit, std::string_view text) {
   const auto size = std::min(text.size(), static_cast<std::size_t>(limit - out));
   return std::copy_n(text.begin(), size, out);
}

} // namespace

CommandProcessor::CommandProcessor(const DeviceInfo &info,
                                   boot::BootloaderEntry *bootloader,
                                   UsbToXxxHandler *usbToXxx,
                                   personality::PersonalitySwitch *personalitySwitch)
   : info_(info)
   , bootloader_(bootloader)
   , usbToXxx_(usbToXxx)
   , personalitySwitch_(personalitySwitch) {
}

std::optional<std::size_t> CommandProcessor::process(art::span<const uint8_t> command, art::span<uint8_t> reply) {
   if(command.empty()) {
      return std::nullopt;
   }
   if(isUsbToXxx(command[0])) {
      if(usbToXxx_ == nullptr) {
         return std::nullopt;
      }
      return usbToXxx_->process(command, reply);
   }
   switch(command[0]) {
   case getInfo:
      return info(reply);
   case getTvcc:
      putU16(reply, usbToXxx_ != nullptr ? usbToXxx_->targetVoltage() : 0);
      return 2;
   case getHardware:
      reply[0] = info_.hardwareId;
      return 1;
   case switchPersonality:
      return personalitySwitch(command, reply);
   case fwUpdate:
      return firmwareUpdate(command, reply);
   default:
      return std::nullopt;
   }
}

void CommandProcessor::hostGone() {
   if(usbToXxx_ != nullptr) {
      usbToXxx_->hostGone();
   }
}

std::size_t CommandProcessor::info(art::span<uint8_t> reply) const {
   putU16(reply, static_cast<uint16_t>(bufferSize));
   uint8_t *const text = reply.data() + 2;
   const uint8_t *const limit = text + maxInfoText;
   uint8_t *end = append(text, limit, "openmupp ");
   end = append(end, limit, info_.version);
   end = append(end, limit, ", board ");
   end = append(end, limit, info_.board);
   *end++ = 0;
   return static_cast<std::size_t>(end - reply.data());
}

std::size_t CommandProcessor::firmwareUpdate(art::span<const uint8_t> command, art::span<uint8_t> reply) {
   const bool accept = bootloader_ != nullptr && command.size() >= 2 && command[1] == fwUpdateKey;
   reply[0] = accept ? fwUpdateAccepted : fwUpdateRefused;
   if(accept) {
      // The main loop resets after the reply delay, also when the host does
      // not read the reply.
      bootloader_->request();
   }
   return 1;
}

std::size_t CommandProcessor::personalitySwitch(art::span<const uint8_t> command, art::span<uint8_t> reply) {
   if(command.size() >= 2 && command[1] == boot::bootloaderCode) {
      const bool accept = bootloader_ != nullptr;
      reply[0] = accept ? switchAccepted : switchRefused;
      if(accept) {
         // As FW_UPDATE: the main loop resets after the reply delay
         bootloader_->request();
      }
      return 1;
   }
   const auto target = command.size() >= 2 ? personality::fromCode(command[1]) : std::nullopt;
   const bool accept = personalitySwitch_ != nullptr && target.has_value();
   reply[0] = accept ? switchAccepted : switchRefused;
   if(accept) {
      // The main loop stores the personality and resets after the reply
      // delay.
      personalitySwitch_->request(*target);
   }
   return 1;
}

} // namespace protocol

#pragma once

// Constants of the Versaloon command protocol, as OpenOCD (vsllink,
// versaloon_internal.h and usbtoxxx_internal.h) uses it.
//
// The host sends one command in one bulk OUT transfer and reads the reply
// in one bulk IN transfer. The first byte is the command:
//
//   0x00-0x0F  common commands. The command fits in one packet.
//   0x20-0x7F  USB_TO_XXX commands: [cmd][length u16][...]. length is the
//              total size, the 3-byte header included, little-endian. The
//              command can span many packets.
//
// The other command bytes are not used.

#include <cstddef>
#include <cstdint>

namespace protocol {

// Common commands
inline constexpr uint8_t getInfo = 0x00;
inline constexpr uint8_t getTvcc = 0x01;
inline constexpr uint8_t getHardware = 0x02;
inline constexpr uint8_t switchPersonality = 0x0E;
inline constexpr uint8_t fwUpdate = 0x0F;

// The key of FW_UPDATE: [0x0F][0xAA]
inline constexpr uint8_t fwUpdateKey = 0xAA;

// Status byte of the FW_UPDATE reply
inline constexpr uint8_t fwUpdateAccepted = 0x00;
inline constexpr uint8_t fwUpdateRefused = 0x01;

// Status byte of the SWITCH_PERSONALITY reply
inline constexpr uint8_t switchAccepted = 0x00;
inline constexpr uint8_t switchRefused = 0x01;

// Command ranges
inline constexpr uint8_t commonLast = 0x0F;
inline constexpr uint8_t usbToXxxFirst = 0x20;
inline constexpr uint8_t usbToXxxLast = 0x7F;

// Size of the [cmd][length u16] header of a USB_TO_XXX command
inline constexpr std::size_t usbToXxxHeaderSize = 3;

// Size of the command buffer and of the reply buffer. GET_INFO reports it:
// the host makes no command and expects no reply larger than this size.
inline constexpr std::size_t bufferSize = 4096;

constexpr bool isCommon(uint8_t command) {
   return command <= commonLast;
}

constexpr bool isUsbToXxx(uint8_t command) {
   return command >= usbToXxxFirst && command <= usbToXxxLast;
}

} // namespace protocol

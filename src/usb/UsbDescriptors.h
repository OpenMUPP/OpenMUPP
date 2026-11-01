// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// USB identity and descriptors of the dongle: a composite device with the
// Versaloon vendor interface and one CDC-ACM serial port.
//
//   Interface 0     vendor class 0xFF, bulk OUT 0x03, bulk IN 0x82
//                   (the OpenOCD vsllink defaults)
//   Interfaces 1-2  CDC-ACM, grouped by an IAD: notification IN 0x81,
//                   bulk OUT 0x04, bulk IN 0x84
//
// VID:PID, the endpoints of interface 0 and the product string are what
// OpenOCD looks for. The CDC endpoints are those of the original firmware.
// On the STM32F1 both directions of an endpoint number share one register
// and one type, so each function has its own endpoint numbers.
//
// Hardware-independent: the host unit tests check the descriptors.
// usb/UsbStack.cpp serves them, with the serial number string from the
// chip UID.

#include "Version.h"

#include <cstddef>
#include <cstdint>
#include <usb/DescriptorBuilder.h>

namespace usb {

namespace desc = art::usb::descriptor;

// OpenOCD defaults (vsllink usb_vid / usb_pid)
inline constexpr uint16_t vendorId = 0x0483;
inline constexpr uint16_t productId = 0xA038;

// The original firmware asks for 200 mA too.
inline constexpr uint16_t maxPowerMilliamps = 200;

inline constexpr uint8_t versaloonInterface = 0;
inline constexpr uint8_t cdcControlInterface = 1;
inline constexpr uint8_t cdcDataInterface = 2;

inline constexpr uint8_t versaloonOutEndpoint = 0x03;
inline constexpr uint8_t versaloonInEndpoint = 0x82;
inline constexpr uint16_t versaloonPacketSize = 64;

inline constexpr uint8_t cdcNotificationEndpoint = 0x81;
// Must not divide the 10-byte SERIAL_STATE notification (see
// art::usb::CdcAcmEndpoints).
inline constexpr uint16_t cdcNotificationPacketSize = 16;
// bInterval of the notification endpoint, ms
inline constexpr uint8_t cdcNotificationInterval = 16;
inline constexpr uint8_t cdcDataOutEndpoint = 0x04;
inline constexpr uint8_t cdcDataInEndpoint = 0x84;
// art::usb::CdcAcmFunctionBase::kDataPacketSize
inline constexpr uint16_t cdcDataPacketSize = 64;

// The highest endpoint number plus 1. The STM32F1 USB peripheral then has a
// 40-byte buffer descriptor table instead of 64 bytes (hw::UsbDevice).
inline constexpr uint8_t endpointCount = 5;
static_assert((versaloonOutEndpoint & 0x0F) < endpointCount && (versaloonInEndpoint & 0x0F) < endpointCount &&
              (cdcNotificationEndpoint & 0x0F) < endpointCount && (cdcDataOutEndpoint & 0x0F) < endpointCount &&
              (cdcDataInEndpoint & 0x0F) < endpointCount);

// String descriptor indexes. Index 0 is the LANGID table.
enum StringIndex : uint8_t {
   stringLanguages = 0,
   stringManufacturer,
   stringProduct,
   stringSerialNumber,
   stringSerialPort,
   stringCount
};

constexpr uint8_t toBcd(uint32_t value) {
   return static_cast<uint8_t>(((value / 10) % 10) << 4 | (value % 10));
}

// Miscellaneous/common/IAD device class: Windows then groups the two CDC
// interfaces through the IAD and binds its CDC driver to the pair.
inline constexpr auto deviceDescriptor = desc::device(desc::DeviceFields{
.bDeviceClass = desc::classcode::kMiscellaneous,
.bDeviceSubClass = desc::classcode::kMiscellaneousSubclassCommon,
.bDeviceProtocol = desc::classcode::kMiscellaneousProtocolIad,
.idVendor = vendorId,
.idProduct = productId,
.bcdDevice = static_cast<uint16_t>(toBcd(VERSION_MAJOR) << 8 | toBcd(VERSION_MINOR)),
.iManufacturer = stringManufacturer,
.iProduct = stringProduct,
.iSerialNumber = stringSerialNumber,
});

inline constexpr auto configurationDescriptor = desc::configuration(
desc::ConfigurationFields{ .maxPowerMilliamps = maxPowerMilliamps },
desc::interfaceDescriptor(desc::InterfaceFields{ versaloonInterface, 0, desc::classcode::kVendorSpecific, 0, 0, 0 },
                          desc::endpoint(versaloonOutEndpoint, art::usb::EndpointType::Bulk, versaloonPacketSize),
                          desc::endpoint(versaloonInEndpoint, art::usb::EndpointType::Bulk, versaloonPacketSize)),
desc::interfaceAssociation(cdcControlInterface, 2, desc::classcode::kCdc, desc::cdc::kSubclassAcm, 0, stringSerialPort),
desc::interfaceDescriptor(
desc::InterfaceFields{ cdcControlInterface, 0, desc::classcode::kCdc, desc::cdc::kSubclassAcm, 0, stringSerialPort },
desc::cdcHeader(),
// No call management by the device. The data interface is 2: the
// original firmware has 1 here, which is wrong.
desc::cdcCallManagement(0, cdcDataInterface),
desc::cdcAcm(),
desc::cdcUnion(cdcControlInterface, cdcDataInterface),
desc::endpoint(cdcNotificationEndpoint, art::usb::EndpointType::Interrupt, cdcNotificationPacketSize, cdcNotificationInterval)),
desc::interfaceDescriptor(desc::InterfaceFields{ cdcDataInterface, 0, desc::classcode::kCdcData, 0, 0, 0 },
                          desc::endpoint(cdcDataOutEndpoint, art::usb::EndpointType::Bulk, cdcDataPacketSize),
                          desc::endpoint(cdcDataInEndpoint, art::usb::EndpointType::Bulk, cdcDataPacketSize)));

inline constexpr auto languageDescriptor = desc::languages(desc::kLanguageEnglishUs);
inline constexpr auto manufacturerDescriptor = desc::stringDescriptor(u"OpenMUPP");
// OpenOCD (vsllink) looks for "Versaloon" in the product string.
inline constexpr auto productDescriptor = desc::stringDescriptor(u"Versaloon");
inline constexpr auto serialPortDescriptor = desc::stringDescriptor(u"OpenMUPP serial port");

// The serial number string: the 96-bit chip UID in hexadecimal, 24 digits.
inline constexpr std::size_t uidBytes = 12;
using SerialNumber = desc::SerialNumber<uidBytes>;

} // namespace usb

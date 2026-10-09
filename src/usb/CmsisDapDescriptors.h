#pragma once

// USB identity and descriptors of the CMSIS-DAP personality: a composite
// device with the CMSIS-DAP v2 interface and one CDC-ACM serial port.
//
//   Interface 0     vendor class 0xFF, bulk OUT 0x03, bulk IN 0x82, 64 bytes.
//                   The interface string contains "CMSIS-DAP": the host
//                   tools find a CMSIS-DAP v2 interface by that string.
//   Interfaces 1-2  CDC-ACM with IAD, as in the Versaloon personality
//
// The endpoint numbers and sizes are those of the Versaloon personality
// (usb/UsbDescriptors.h), so the packet memory budget does not change. Only
// one personality is active in an image. The Versaloon identity stays the
// default.
//
// Hardware-independent: the host unit tests check the descriptors.
// usb/UsbStack.cpp serves them, with the serial number string from the
// chip UID.

#include "usb/UsbDescriptors.h"

#include <cstdint>
#include <usb/DescriptorBuilder.h>

namespace usb::dap {

inline constexpr uint16_t vendorId = 0x0403;
inline constexpr uint16_t productId = 0x9B3C;

inline constexpr uint8_t dapInterface = 0;
inline constexpr uint8_t outEndpoint = versaloonOutEndpoint;
inline constexpr uint8_t inEndpoint = versaloonInEndpoint;
// One CMSIS-DAP command or response is one packet.
inline constexpr uint16_t packetSize = 64;

// String descriptor indexes. Index 0 is the LANGID table.
enum StringIndex : uint8_t {
   stringLanguages = 0,
   stringManufacturer,
   stringProduct,
   stringSerialNumber,
   stringSerialPort,
   stringInterface,
   stringCount
};

// The USB packet memory of the STM32F1 in bytes
inline constexpr unsigned packetMemorySize = 512;
// The buffer descriptor table, EP0 (64 bytes each way), the double-buffered
// OUT endpoint of the DAP interface, the IN endpoint, and the CDC-ACM
// endpoints.
inline constexpr unsigned packetMemoryUsed =
endpointCount * 8 + 2 * 64 + 2 * packetSize + packetSize + cdcNotificationPacketSize + 2 * cdcDataPacketSize;
static_assert(packetMemoryUsed <= packetMemorySize);

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
desc::interfaceDescriptor(desc::InterfaceFields{ dapInterface, 0, desc::classcode::kVendorSpecific, 0, 0, stringInterface },
                          desc::endpoint(outEndpoint, art::usb::EndpointType::Bulk, packetSize),
                          desc::endpoint(inEndpoint, art::usb::EndpointType::Bulk, packetSize)),
desc::interfaceAssociation(cdcControlInterface, 2, desc::classcode::kCdc, desc::cdc::kSubclassAcm, 0, stringSerialPort),
desc::interfaceDescriptor(
desc::InterfaceFields{ cdcControlInterface, 0, desc::classcode::kCdc, desc::cdc::kSubclassAcm, 0, stringSerialPort },
desc::cdcHeader(),
desc::cdcCallManagement(0, cdcDataInterface),
desc::cdcAcm(),
desc::cdcUnion(cdcControlInterface, cdcDataInterface),
desc::endpoint(cdcNotificationEndpoint, art::usb::EndpointType::Interrupt, cdcNotificationPacketSize, cdcNotificationInterval)),
desc::interfaceDescriptor(desc::InterfaceFields{ cdcDataInterface, 0, desc::classcode::kCdcData, 0, 0, 0 },
                          desc::endpoint(cdcDataOutEndpoint, art::usb::EndpointType::Bulk, cdcDataPacketSize),
                          desc::endpoint(cdcDataInEndpoint, art::usb::EndpointType::Bulk, cdcDataPacketSize)));

inline constexpr auto languageDescriptor = desc::languages(desc::kLanguageEnglishUs);
inline constexpr auto manufacturerDescriptor = desc::stringDescriptor(u"Versaloon");
// pyOCD looks for "CMSIS-DAP" in the product string.
inline constexpr auto productDescriptor = desc::stringDescriptor(u"Versaloon CMSIS-DAP");
inline constexpr auto serialPortDescriptor = desc::stringDescriptor(u"Versaloon serial port");
// A CMSIS-DAP v2 interface is the vendor interface with "CMSIS-DAP" in this
// string.
inline constexpr auto interfaceStringDescriptor = desc::stringDescriptor(u"Versaloon CMSIS-DAP v2");

} // namespace usb::dap

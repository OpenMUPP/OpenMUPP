// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// USB identity and descriptors of the CMSIS-DAP personality: a composite
// device with the CMSIS-DAP v2 interface and one CDC-ACM serial port.
//
//   Interface 0     vendor class 0xFF, bulk OUT 0x03, bulk IN 0x82, 64 bytes.
//                   The interface string contains "CMSIS-DAP": the host
//                   tools find a CMSIS-DAP v2 interface by that string.
//   Interfaces 1-2  CDC-ACM with IAD, as in the Versaloon personality
//
// Windows 8.1 and later bind WinUSB to interface 0 with no driver
// installation: bcdUSB 2.01 makes Windows read the BOS, the BOS has the
// MS OS 2.0 platform capability, and the MS OS 2.0 descriptor set gives
// interface 0 the compatible ID "WINUSB" and a device interface GUID.
// UsbStack answers the vendor request for the set. The CDC interfaces get
// no MS OS 2.0 descriptors: Windows 10 and later bind their own CDC driver
// (usbser.sys).
//
// The endpoint numbers and sizes are those of the Versaloon personality
// (usb/UsbDescriptors.h), so the packet memory budget does not change. Only
// one personality is active in an image. The Versaloon identity stays the
// default.
//
// Hardware-independent: the host unit tests check the descriptors.
// The USB stack serves them, with the serial number string from the chip
// UID, in a CMSIS-DAP image.

#include "usb/UsbDescriptors.h"

#include <cstdint>
#include <usb/DescriptorBuilder.h>
#include <usb/MsOs20.h>

namespace usb::dap {

inline constexpr uint16_t vendorId = 0x1209;
inline constexpr uint16_t productId = 0x0001;

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

// USB 2.01: a USB 2.0 device with a BOS descriptor
inline constexpr uint16_t bcdUsb = 0x0201;

// bRequest of the vendor request that reads the MS OS 2.0 descriptor set.
// The device has no other device vendor requests.
inline constexpr uint8_t msOs20VendorCode = 0x20;

// The device interface GUID of a CMSIS-DAP v2 interface, the GUID of the
// Arm CMSIS-DAP and DAPLink firmware. Tools that look for a CMSIS-DAP v2
// probe by this GUID find the probe.
inline constexpr char16_t dapInterfaceGuid[] = u"{CDB3B5AD-293B-4663-AA36-1AAE46463776}";

// Miscellaneous/common/IAD device class: Windows then groups the two CDC
// interfaces through the IAD and binds its CDC driver to the pair.
inline constexpr auto deviceDescriptor = desc::device(desc::DeviceFields{
.bcdUSB = bcdUsb,
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
inline constexpr auto manufacturerDescriptor = desc::stringDescriptor(u"OpenMUPP");
// pyOCD looks for "CMSIS-DAP" in the product string.
inline constexpr auto productDescriptor = desc::stringDescriptor(u"OpenMUPP CMSIS-DAP");
inline constexpr auto serialPortDescriptor = desc::stringDescriptor(u"OpenMUPP serial port");
// A CMSIS-DAP v2 interface is the vendor interface with "CMSIS-DAP" in this
// string.
inline constexpr auto interfaceStringDescriptor = desc::stringDescriptor(u"OpenMUPP CMSIS-DAP v2");

// The MS OS 2.0 descriptor set: WinUSB for interface 0 only. The device
// has one configuration, so the function subset follows the set header
// directly.
inline constexpr auto msOs20DescriptorSet =
art::usb::msos20::descriptorSet(art::usb::msos20::kWindows81,
                                art::usb::msos20::functionSubset(dapInterface,
                                                                 art::usb::msos20::compatibleId("WINUSB"),
                                                                 art::usb::msos20::deviceInterfaceGuid(dapInterfaceGuid)));

inline constexpr auto bosDescriptor = desc::bos(art::usb::msos20::platformCapability(msOs20VendorCode, msOs20DescriptorSet));

} // namespace usb::dap

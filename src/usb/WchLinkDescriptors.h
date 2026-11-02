// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// USB identity and descriptors of the WCH-Link personality: one vendor
// interface with the two bulk pairs of the WCH-LinkE in RISC-V mode
// (docs/wch-link-protocol.md, "USB interface").
//
//   Interface 0  vendor class 0xFF, 64 bytes on each endpoint:
//                bulk OUT 0x01, bulk IN 0x81   commands and responses
//                bulk OUT 0x02, bulk IN 0x82   bulk data (memory read,
//                                              flash program)
//
// wlink and minichlink use these endpoint numbers. The CDC interfaces of
// the WCH-LinkE (the UART of the probe) are not in scope (epic #63), and
// they do not fit the 512 bytes of packet memory with the two bulk pairs.
//
// USB ID: the default is an own ID. The WCH ID 1a86:8010 is a build option
// (MUPP_WCH_LINK_WCH_USB_ID) for personal use only: the WCH vendor ID
// belongs to WCH and must not be in release artifacts. wlink and minichlink
// find a probe by the WCH ID. With the own ID, they need a flag or a patch.
// The own ID is the pid.codes test ID 1209:0001 until the project has its
// own product ID (the AVR personality plan requests one).
//
// On the STM32F1, both directions of an endpoint number share one register
// and one type. Both pairs are bulk, so this works, but no endpoint can be
// double-buffered.
//
// Hardware-independent: the host unit tests check the descriptors.

#include "usb/UsbDescriptors.h"

#include <cstdint>
#include <usb/DescriptorBuilder.h>

namespace usb::wch {

#if defined(MUPP_WCH_LINK_WCH_USB_ID)
inline constexpr uint16_t vendorId = 0x1A86;
inline constexpr uint16_t productId = 0x8010;
#else
inline constexpr uint16_t vendorId = 0x1209;
inline constexpr uint16_t productId = 0x0001;
#endif

inline constexpr uint8_t wchInterface = 0;
inline constexpr uint8_t commandOutEndpoint = 0x01;
inline constexpr uint8_t commandInEndpoint = 0x81;
inline constexpr uint8_t dataOutEndpoint = 0x02;
inline constexpr uint8_t dataInEndpoint = 0x82;
inline constexpr uint16_t packetSize = 64;

// The highest endpoint number plus 1
inline constexpr uint8_t endpointCount = 3;

// String descriptor indexes. Index 0 is the LANGID table.
enum StringIndex : uint8_t { stringLanguages = 0, stringManufacturer, stringProduct, stringSerialNumber, stringCount };

// The USB packet memory of the STM32F1 in bytes
inline constexpr unsigned packetMemorySize = 512;
// The buffer descriptor table, EP0 (64 bytes each way) and the four bulk
// endpoints, all single-buffered
inline constexpr unsigned packetMemoryUsed = endpointCount * 8 + 2 * 64 + 4 * packetSize;
static_assert(packetMemoryUsed <= packetMemorySize);

inline constexpr auto deviceDescriptor = desc::device(desc::DeviceFields{
.idVendor = vendorId,
.idProduct = productId,
.bcdDevice = static_cast<uint16_t>(toBcd(VERSION_MAJOR) << 8 | toBcd(VERSION_MINOR)),
.iManufacturer = stringManufacturer,
.iProduct = stringProduct,
.iSerialNumber = stringSerialNumber,
});

inline constexpr auto configurationDescriptor =
desc::configuration(desc::ConfigurationFields{ .maxPowerMilliamps = maxPowerMilliamps },
                    desc::interfaceDescriptor(desc::InterfaceFields{ wchInterface, 0, desc::classcode::kVendorSpecific, 0, 0, 0 },
                                              desc::endpoint(commandOutEndpoint, art::usb::EndpointType::Bulk, packetSize),
                                              desc::endpoint(commandInEndpoint, art::usb::EndpointType::Bulk, packetSize),
                                              desc::endpoint(dataOutEndpoint, art::usb::EndpointType::Bulk, packetSize),
                                              desc::endpoint(dataInEndpoint, art::usb::EndpointType::Bulk, packetSize)));

inline constexpr auto languageDescriptor = desc::languages(desc::kLanguageEnglishUs);
inline constexpr auto manufacturerDescriptor = desc::stringDescriptor(u"OpenMUPP");
#if defined(MUPP_WCH_LINK_WCH_USB_ID)
// probe-rs finds a WCH-Link only by this exact product string (get_wlink_info()
// in probe-rs 0.32.0). The manufacturer "OpenMUPP" tells it from a real WCH-LinkE.
inline constexpr auto productDescriptor = desc::stringDescriptor(u"WCH-Link");
#else
inline constexpr auto productDescriptor = desc::stringDescriptor(u"OpenMUPP WCH-Link");
#endif

} // namespace usb::wch

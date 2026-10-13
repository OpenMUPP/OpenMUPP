// The descriptors and the CMSIS-DAP function on the libArT USB device stack,
// with the fake controller of libArT as the host.

#include "CmsisDapBench.h"

#include <doctest/doctest.h>
#include <usb/MsOs20.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using art::usb::DescriptorType;
using art::usb::EndpointType;
using vsltest::DapBench;
using vsltest::dapIn;
using vsltest::dapOut;
using Bytes = std::vector<uint8_t>;

Bytes bytes(const auto &descriptor) {
   return { descriptor.begin(), descriptor.end() };
}

} // namespace

TEST_CASE("The host reads the descriptors of the CMSIS-DAP device") {
   DapBench bench;
   bench.controller().busReset();

   CHECK(bench.descriptor(DescriptorType::Device, 0, 18).data == bytes(usb::dap::deviceDescriptor));

   const auto header = bench.descriptor(DescriptorType::Configuration, 0, 9).data;
   REQUIRE(header.size() == 9);
   const uint16_t totalLength = static_cast<uint16_t>(header[2] | header[3] << 8);
   CHECK(bench.descriptor(DescriptorType::Configuration, 0, totalLength).data == bytes(usb::dap::configurationDescriptor));

   CHECK(bench.descriptor(DescriptorType::String, usb::dap::stringProduct, 255).data == bytes(usb::dap::productDescriptor));
   CHECK(bench.descriptor(DescriptorType::String, usb::dap::stringInterface, 255).data == bytes(usb::dap::interfaceStringDescriptor));
   const auto serial = bench.descriptor(DescriptorType::String, usb::dap::stringSerialNumber, 255).data;
   REQUIRE(serial.size() == 50);
   CHECK(serial[2] == '1');
   CHECK(serial[4] == '2');
   CHECK(serial[48] == '4');
}

TEST_CASE("Windows reads the BOS and the MS OS 2.0 descriptor set before SET_CONFIGURATION") {
   DapBench bench;
   auto &controller = bench.controller();
   controller.busReset();
   bench.request(vsltest::setAddress, 5);

   // The BOS header, then the whole BOS
   const auto header = bench.descriptor(DescriptorType::Bos, 0, 5).data;
   REQUIRE(header.size() == 5);
   const uint16_t bosLength = static_cast<uint16_t>(header[2] | header[3] << 8);
   CHECK(bench.descriptor(DescriptorType::Bos, 0, bosLength).data == bytes(usb::dap::bosDescriptor));

   // The vendor request with the vendor code of the platform capability:
   // bmRequestType 0xC0, wIndex 7. The set is longer than one packet.
   bool ok = false;
   const auto set = controller.controlIn(vsltest::setupPacket(0xC0, usb::dap::msOs20VendorCode, 0, art::usb::msos20::kDescriptorIndex,
                                                              static_cast<uint16_t>(usb::dap::msOs20DescriptorSet.size())),
                                         &ok);
   CHECK(ok);
   CHECK_FALSE(set.stalled);
   CHECK(set.data == bytes(usb::dap::msOs20DescriptorSet));
   CHECK(set.packetSizes == std::vector<std::size_t>{ 64, 64, 42 });

   // Another vendor code stalls.
   CHECK(
   controller.controlIn(vsltest::setupPacket(0xC0, static_cast<uint8_t>(usb::dap::msOs20VendorCode + 1), 0, 7, 16)).stalled);

   bench.request(vsltest::setConfiguration, 1);
}

TEST_CASE("SET_CONFIGURATION opens the endpoints of all three CMSIS-DAP interfaces") {
   DapBench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   struct Expected {
      uint8_t address;
      EndpointType type;
      uint16_t maxPacketSize;
   };
   for(const Expected &expected : { Expected{ 0x03, EndpointType::Bulk, 64 }, Expected{ 0x82, EndpointType::Bulk, 64 },
                                    Expected{ 0x81, EndpointType::Interrupt, 16 },
                                    Expected{ 0x04, EndpointType::Bulk, 64 }, Expected{ 0x84, EndpointType::Bulk, 64 } }) {
      CAPTURE(int(expected.address));
      const auto endpoint = controller.endpoint(expected.address);
      REQUIRE(endpoint.has_value());
      CHECK(endpoint->type == expected.type);
      CHECK(endpoint->maxPacketSize == expected.maxPacketSize);
   }
   CHECK(bench.function.isConfigured());
   CHECK(controller.isArmed(dapOut));
}

TEST_CASE("DAP_Info over USB: one response packet per command") {
   DapBench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   REQUIRE(bench.write({ 0x00, 0x02 }));
   const auto product = bench.read();
   REQUIRE(product.has_value());
   CHECK(*product == Bytes{ 0x00, 0x08, 'P', 'r', 'o', 'd', 'u', 'c', 't', 0 });
   // No ZLP follows a short response.
   CHECK_FALSE(bench.read().has_value());
   CHECK(controller.isArmed(dapOut));

   REQUIRE(bench.write({ 0x00, 0xFF }));
   CHECK(bench.read() == Bytes{ 0x00, 0x02, 0x40, 0x00 });
}

TEST_CASE("DAP_Info reports the packet count of the function") {
   DapBench bench;
   bench.enumerate();

   REQUIRE(bench.write({ 0x00, 0xFE }));
   CHECK(bench.read() == Bytes{ 0x00, 0x01, static_cast<uint8_t>(cmsisdap::Processor::packetCount) });
}

TEST_CASE("A response of the full packet size ends without a ZLP") {
   const std::string longText(100, 'x');
   DapBench bench(longText.c_str());
   bench.enumerate();

   REQUIRE(bench.write({ 0x00, 0x02 }));
   const auto response = bench.read();
   REQUIRE(response.has_value());
   CHECK(response->size() == 64);
   CHECK_FALSE(bench.read().has_value());
   CHECK(bench.controller().isArmed(dapOut));
}

TEST_CASE("An empty OUT packet is ignored") {
   DapBench bench;
   bench.enumerate();

   REQUIRE(bench.write({}));
   CHECK_FALSE(bench.read().has_value());
   CHECK(bench.controller().isArmed(dapOut));
   REQUIRE(bench.write({ 0x09, 0x05, 0x00 }));
   CHECK(bench.read() == Bytes{ 0x09, 0x00 });
}

TEST_CASE("The host sends packets before it reads: the responses come in order") {
   DapBench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   // The first response is on its way; the next three wait in the
   // processor. Then all four slots are in use and the OUT endpoint NAKs.
   REQUIRE(bench.write({ 0x00, 0xFE }));
   REQUIRE(bench.write({ 0x00, 0xFF }));
   REQUIRE(bench.write({ 0x09, 0x05, 0x00 }));
   CHECK(controller.isArmed(dapOut));
   REQUIRE(bench.write({ 0x01, 0x00, 0x01 }));
   CHECK_FALSE(controller.isArmed(dapOut));
   CHECK_FALSE(bench.write({ 0x00, 0xFE }));

   // Each response sent frees one slot and arms the OUT endpoint.
   CHECK(bench.read() == Bytes{ 0x00, 0x01, 0x04 });
   CHECK(controller.isArmed(dapOut));
   CHECK(bench.read() == Bytes{ 0x00, 0x02, 0x40, 0x00 });
   CHECK(bench.read() == Bytes{ 0x09, 0x00 });
   CHECK(bench.read() == Bytes{ 0x01, 0x00 });
   CHECK_FALSE(bench.read().has_value());
   CHECK(controller.isArmed(dapOut));
}

TEST_CASE("A command that waits at the NAKing OUT endpoint runs after a response is sent") {
   DapBench bench;
   bench.enumerate();

   for(int i = 0; i < 4; ++i) {
      REQUIRE(bench.write({ 0x09, static_cast<uint8_t>(i + 1), 0x00 }));
   }
   CHECK_FALSE(bench.write({ 0x09, 0x09, 0x00 }));
   CHECK(bench.read().has_value());
   REQUIRE(bench.write({ 0x09, 0x09, 0x00 }));
   for(int i = 0; i < 4; ++i) {
      CHECK(bench.read() == Bytes{ 0x09, 0x00 });
   }
   CHECK_FALSE(bench.read().has_value());
   CHECK(bench.platform.delays == std::vector<uint32_t>{ 1, 2, 3, 4, 9 });
}

TEST_CASE("DAP_QueueCommands packets wait and run with the next command") {
   DapBench bench;
   bench.enumerate();

   REQUIRE(bench.write({ 0x7E, 0x01, 0x00, 0xFE }));
   // A queued packet has no response.
   CHECK_FALSE(bench.read().has_value());
   CHECK(bench.controller().isArmed(dapOut));

   REQUIRE(bench.write({ 0x00, 0xFF }));
   CHECK(bench.read() == Bytes{ 0x7F, 0x01, 0x00, 0x01, 0x04 });
   CHECK(bench.read() == Bytes{ 0x00, 0x02, 0x40, 0x00 });
}

TEST_CASE("Three queued packets and a command fill the four slots") {
   DapBench bench;
   bench.enumerate();

   for(int i = 0; i < 3; ++i) {
      REQUIRE(bench.write({ 0x7E, 0x01, 0x00, 0xFE }));
      CHECK(bench.controller().isArmed(dapOut));
   }
   REQUIRE(bench.write({ 0x00, 0xFF }));
   for(int i = 0; i < 3; ++i) {
      CHECK(bench.read() == Bytes{ 0x7F, 0x01, 0x00, 0x01, 0x04 });
   }
   CHECK(bench.read() == Bytes{ 0x00, 0x02, 0x40, 0x00 });
   CHECK(bench.controller().isArmed(dapOut));
}

TEST_CASE("A bus reset disconnects the target and drops the queue and the responses") {
   DapBench bench;
   bench.enumerate();

   REQUIRE(bench.write({ 0x02, 0x01 }));
   CHECK(bench.read() == Bytes{ 0x02, 0x01 });
   CHECK(bench.processor.connected());
   CHECK(bench.swd.inits == 1);

   REQUIRE(bench.write({ 0x7E, 0x01, 0x00, 0xFE }));
   REQUIRE(bench.write({ 0x00, 0xFF }));
   // The host does not read the response and resets the bus.
   bench.enumerate();
   CHECK_FALSE(bench.processor.connected());
   CHECK(bench.swd.finis == 1);
   CHECK_FALSE(bench.processor.hasResponse());
   CHECK_FALSE(bench.read().has_value());

   // The dropped queue does not run with the next command.
   REQUIRE(bench.write({ 0x00, 0xFE }));
   CHECK(bench.read() == Bytes{ 0x00, 0x01, 0x04 });
   CHECK_FALSE(bench.read().has_value());
}

TEST_CASE("A bus reset while the OUT endpoint NAKs: the next command works") {
   DapBench bench;
   bench.enumerate();

   for(int i = 0; i < 4; ++i) {
      REQUIRE(bench.write({ 0x00, 0xFE }));
   }
   CHECK_FALSE(bench.controller().isArmed(dapOut));

   bench.enumerate();
   CHECK(bench.controller().isArmed(dapOut));
   REQUIRE(bench.write({ 0x00, 0xFF }));
   CHECK(bench.read() == Bytes{ 0x00, 0x02, 0x40, 0x00 });
}

TEST_CASE("A suspend disconnects the target") {
   DapBench bench;
   bench.enumerate();

   REQUIRE(bench.write({ 0x02, 0x01 }));
   CHECK(bench.read() == Bytes{ 0x02, 0x01 });
   REQUIRE(bench.processor.connected());

   bench.controller().busSuspend();
   CHECK_FALSE(bench.processor.connected());
   CHECK(bench.swd.finis == 1);
}

TEST_CASE("Four queued packets run at once and each one gets its response") {
   DapBench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   for(int i = 0; i < 3; ++i) {
      REQUIRE(bench.write({ 0x7E, 0x01, 0x09, static_cast<uint8_t>(i + 1), 0x00 }));
   }
   CHECK(bench.platform.delays.empty());
   // The fourth queued packet takes the last slot and runs the queue.
   REQUIRE(bench.write({ 0x7E, 0x01, 0x09, 0x04, 0x00 }));
   CHECK(bench.platform.delays == std::vector<uint32_t>{ 1, 2, 3, 4 });
   CHECK_FALSE(controller.isArmed(dapOut));

   CHECK(bench.read() == Bytes{ 0x7F, 0x01, 0x09, 0x00 });
   CHECK(controller.isArmed(dapOut));
   REQUIRE(bench.write({ 0x00, 0xFF }));
   for(int i = 0; i < 3; ++i) {
      CHECK(bench.read() == Bytes{ 0x7F, 0x01, 0x09, 0x00 });
   }
   CHECK(bench.read() == Bytes{ 0x00, 0x02, 0x40, 0x00 });
   CHECK_FALSE(bench.read().has_value());
   CHECK(controller.isArmed(dapOut));
}

TEST_CASE("A suspend and resume with a response on its way: the next command answers in order") {
   DapBench bench;
   bench.enumerate();

   // The response is on its way when the bus suspends.
   REQUIRE(bench.write({ 0x00, 0xFE }));
   bench.controller().busSuspend();
   bench.controller().busResume();
   CHECK(bench.controller().isArmed(dapOut));

   REQUIRE(bench.write({ 0x00, 0xFF }));
   REQUIRE(bench.write({ 0x09, 0x05, 0x00 }));
   CHECK(bench.read() == Bytes{ 0x00, 0x01, 0x04 });
   CHECK(bench.read() == Bytes{ 0x00, 0x02, 0x40, 0x00 });
   CHECK(bench.read() == Bytes{ 0x09, 0x00 });
   CHECK_FALSE(bench.read().has_value());
}

TEST_CASE("A suspend and resume keep the responses in order") {
   DapBench bench;
   bench.enumerate();

   REQUIRE(bench.write({ 0x00, 0xFE }));
   REQUIRE(bench.write({ 0x00, 0xFF }));
   REQUIRE(bench.write({ 0x09, 0x05, 0x00 }));
   REQUIRE(bench.write({ 0x01, 0x00, 0x01 }));
   CHECK_FALSE(bench.controller().isArmed(dapOut));

   bench.controller().busSuspend();
   bench.controller().busResume();
   CHECK(bench.read() == Bytes{ 0x00, 0x01, 0x04 });
   CHECK(bench.controller().isArmed(dapOut));
   CHECK(bench.read() == Bytes{ 0x00, 0x02, 0x40, 0x00 });
   CHECK(bench.read() == Bytes{ 0x09, 0x00 });
   CHECK(bench.read() == Bytes{ 0x01, 0x00 });
   CHECK_FALSE(bench.read().has_value());
}

TEST_CASE("A command that arrives while a command runs waits for it") {
   DapBench bench;
   bench.enumerate();
   REQUIRE(bench.write({ 0x02, 0x01 }));
   CHECK(bench.read() == Bytes{ 0x02, 0x01 });

   // The OUT endpoint is armed while the block runs.
   bool sent = false;
   bench.swd.onTransfer = [&bench, &sent] {
      if(bench.swd.transfers == 2) {
         sent = bench.send({ 0x09, 0x07, 0x00 });
         CHECK(bench.platform.delays.empty());
      }
   };
   // DAP_TransferBlock: 4 reads of DP RDBUFF
   REQUIRE(bench.write({ 0x06, 0x00, 0x04, 0x00, 0x0E }));
   CHECK(sent);
   CHECK(bench.platform.delays == std::vector<uint32_t>{ 7 });
   const auto block = bench.read();
   REQUIRE(block.has_value());
   CHECK(Bytes(block->begin(), block->begin() + 4) == Bytes{ 0x06, 0x04, 0x00, 0x01 });
   CHECK(bench.read() == Bytes{ 0x09, 0x00 });
   CHECK_FALSE(bench.read().has_value());
   CHECK(bench.controller().isArmed(dapOut));
}

TEST_CASE("DAP_TransferAbort over USB stops a running DAP_TransferBlock") {
   DapBench bench;
   bench.enumerate();
   REQUIRE(bench.write({ 0x02, 0x01 }));
   CHECK(bench.read() == Bytes{ 0x02, 0x01 });

   bool sent = false;
   bench.swd.onTransfer = [&bench, &sent] {
      if(bench.swd.transfers == 2) {
         sent = bench.send({ 0x07 });
      }
   };
   // DAP_TransferBlock: 6 reads of DP CTRL/STAT
   REQUIRE(bench.write({ 0x06, 0x00, 0x06, 0x00, 0x06 }));
   CHECK(sent);
   CHECK(bench.swd.transfers == 2);
   const auto block = bench.read();
   REQUIRE(block.has_value());
   CHECK(*block == Bytes{ 0x06, 0x02, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0 });
   // The abort has no response.
   CHECK_FALSE(bench.read().has_value());
   CHECK(bench.controller().isArmed(dapOut));
}

TEST_CASE("DAP_TransferAbort over USB between commands has no response") {
   DapBench bench;
   bench.enumerate();
   REQUIRE(bench.write({ 0x07 }));
   CHECK_FALSE(bench.read().has_value());
   CHECK(bench.controller().isArmed(dapOut));
   REQUIRE(bench.write({ 0x00, 0xFE }));
   CHECK(bench.read() == Bytes{ 0x00, 0x01, 0x04 });
}

TEST_CASE("A response goes out before the next stored packet runs") {
   DapBench bench;
   bench.enumerate();
   REQUIRE(bench.write({ 0x02, 0x01 }));
   CHECK(bench.read() == Bytes{ 0x02, 0x01 });

   // Two packets arrive before the main loop runs.
   REQUIRE(bench.send({ 0x00, 0xFE }));
   REQUIRE(bench.send({ 0x06, 0x00, 0x01, 0x00, 0x06 }));
   std::optional<Bytes> first;
   bench.swd.onTransfer = [&bench, &first] { first = bench.read(); };
   bench.mainLoop();
   CHECK(first == Bytes{ 0x00, 0x01, 0x04 });
   CHECK(bench.read() == Bytes{ 0x06, 0x01, 0x00, 0x01, 0, 0, 0, 0 });
}

TEST_CASE("Stored commands run one at a time, and each response goes out during the next command") {
   DapBench bench;
   bench.enumerate();
   REQUIRE(bench.write({ 0x02, 0x01 }));
   CHECK(bench.read() == Bytes{ 0x02, 0x01 });

   // The events wait for the main loop, as on the target. Three
   // DAP_TransferBlock packets of one read each are stored.
   bench.controller().queueEvents(true);
   const Bytes block{ 0x06, 0x00, 0x01, 0x00, 0x06 };
   for(int i = 0; i < 3; ++i) {
      REQUIRE(bench.send(block));
      bench.controller().deliverQueuedEvents();
   }
   // The host reads a response during each command. The IN endpoint is
   // free again only after the main loop delivered the InComplete event of
   // the last response.
   const Bytes response{ 0x06, 0x01, 0x00, 0x01, 0, 0, 0, 0 };
   std::vector<std::optional<Bytes>> readDuring;
   bench.swd.onTransfer = [&bench, &readDuring] { readDuring.push_back(bench.read()); };
   bench.mainLoop();
   REQUIRE(readDuring.size() == 3);
   CHECK_FALSE(readDuring[0].has_value());
   CHECK(readDuring[1] == response);
   CHECK(readDuring[2] == response);
   CHECK(bench.read() == response);
   CHECK_FALSE(bench.read().has_value());
}

TEST_CASE("A packet before a suspend runs after the resume") {
   DapBench bench;
   bench.enumerate();

   // The packet and the suspend come in the same poll of the controller.
   REQUIRE(bench.send({ 0x09, 0x05, 0x00 }));
   bench.controller().busSuspend();
   bench.mainLoop();
   CHECK(bench.platform.delays.empty());
   CHECK_FALSE(bench.read().has_value());

   bench.controller().busResume();
   bench.mainLoop();
   CHECK(bench.platform.delays == std::vector<uint32_t>{ 5 });
   CHECK(bench.read() == Bytes{ 0x09, 0x00 });
}

TEST_CASE("DAP_TransferAbort waits at the OUT endpoint when all slots are in use") {
   DapBench bench;
   bench.enumerate();
   REQUIRE(bench.write({ 0x02, 0x01 }));
   CHECK(bench.read() == Bytes{ 0x02, 0x01 });

   bool abortSent = true;
   bench.swd.onTransfer = [&bench, &abortSent] {
      if(bench.swd.transfers == 1) {
         // The block and three more packets use the four slots.
         for(int i = 0; i < 3; ++i) {
            CHECK(bench.send({ 0x09, 0x01, 0x00 }));
         }
         abortSent = bench.send({ 0x07 });
      }
   };
   REQUIRE(bench.write({ 0x06, 0x00, 0x02, 0x00, 0x06 }));
   CHECK_FALSE(abortSent);
   // The block was not stopped.
   const auto block = bench.read();
   REQUIRE(block.has_value());
   CHECK((*block)[1] == 0x02);
}

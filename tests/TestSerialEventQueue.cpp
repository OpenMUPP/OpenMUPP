// The USB event queue with the serial-only mode of UsbStack::pollSerial().

#include "usb/SerialEventQueue.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using hw::usb::Event;
using hw::usb::EventType;
using Queue = usb::SerialEventQueue<16>;

Event event(EventType type, uint8_t endpoint = 0) {
   return Event{ type, endpoint, 0, {} };
}

// Pops all events that try_pop() gives.
std::vector<Event> popAll(Queue &queue) {
   std::vector<Event> events;
   Event out{};
   while(queue.try_pop(out)) {
      events.push_back(out);
   }
   return events;
}

} // namespace

TEST_CASE("SerialEventQueue: the serial port events are the packets of the CDC endpoints") {
   CHECK(Queue::isSerialEvent(event(EventType::InComplete, usb::cdcDataInEndpoint)));
   CHECK(Queue::isSerialEvent(event(EventType::Out, usb::cdcDataOutEndpoint)));
   CHECK(Queue::isSerialEvent(event(EventType::InComplete, usb::cdcNotificationEndpoint)));
   CHECK_FALSE(Queue::isSerialEvent(event(EventType::Out, usb::versaloonOutEndpoint)));
   CHECK_FALSE(Queue::isSerialEvent(event(EventType::InComplete, usb::versaloonInEndpoint)));
   CHECK_FALSE(Queue::isSerialEvent(event(EventType::InComplete, 0x80)));
   CHECK_FALSE(Queue::isSerialEvent(event(EventType::Out, 0x00)));
   CHECK_FALSE(Queue::isSerialEvent(event(EventType::Setup)));
   CHECK_FALSE(Queue::isSerialEvent(event(EventType::Reset)));
   CHECK_FALSE(Queue::isSerialEvent(event(EventType::Suspend)));
   CHECK_FALSE(Queue::isSerialEvent(event(EventType::Resume)));
}

TEST_CASE("SerialEventQueue: without the serial-only mode, all events come out in order") {
   Queue queue;
   REQUIRE(queue.try_push(event(EventType::Setup)));
   REQUIRE(queue.try_push(event(EventType::InComplete, usb::cdcDataInEndpoint)));
   REQUIRE(queue.try_push(event(EventType::Out, usb::versaloonOutEndpoint)));
   const auto events = popAll(queue);
   REQUIRE(events.size() == 3);
   CHECK(events[0].type == EventType::Setup);
   CHECK(events[1].endpoint == usb::cdcDataInEndpoint);
   CHECK(events[2].endpoint == usb::versaloonOutEndpoint);
   CHECK(queue.empty());
}

TEST_CASE("SerialEventQueue: the serial-only mode stops at the first other event") {
   Queue queue;
   REQUIRE(queue.try_push(event(EventType::InComplete, usb::cdcDataInEndpoint)));
   REQUIRE(queue.try_push(event(EventType::Out, usb::cdcDataOutEndpoint)));
   REQUIRE(queue.try_push(event(EventType::Reset)));
   REQUIRE(queue.try_push(event(EventType::InComplete, usb::cdcDataInEndpoint)));
   queue.setSerialOnly(true);

   const auto serial = popAll(queue);
   REQUIRE(serial.size() == 2);
   CHECK(serial[0].type == EventType::InComplete);
   CHECK(serial[1].type == EventType::Out);
   // The reset and the event after it wait.
   CHECK(queue.size() == 2);
   CHECK(popAll(queue).empty());

   queue.setSerialOnly(false);
   const auto rest = popAll(queue);
   REQUIRE(rest.size() == 2);
   CHECK(rest[0].type == EventType::Reset);
   CHECK(rest[1].endpoint == usb::cdcDataInEndpoint);
}

TEST_CASE("SerialEventQueue: the serial-only mode gives nothing when a SETUP is at the front") {
   Queue queue;
   REQUIRE(queue.try_push(event(EventType::Setup)));
   REQUIRE(queue.try_push(event(EventType::InComplete, usb::cdcDataInEndpoint)));
   queue.setSerialOnly(true);
   CHECK(popAll(queue).empty());
   Event front{};
   REQUIRE(queue.peek(front));
   CHECK(front.type == EventType::Setup);
}

TEST_CASE("SerialEventQueue: the serial-only mode on an empty queue") {
   Queue queue;
   queue.setSerialOnly(true);
   CHECK(popAll(queue).empty());
   REQUIRE(queue.try_push(event(EventType::InComplete, usb::cdcDataInEndpoint)));
   CHECK(popAll(queue).size() == 1);
}

TEST_CASE("SerialEventQueue: the serial-only mode also gives the events of the command endpoints") {
   Queue queue;
   queue.setCommandEndpoints(0x03, 0x82);
   CHECK(queue.isServiceEvent(event(EventType::Out, 0x03)));
   CHECK(queue.isServiceEvent(event(EventType::InComplete, 0x82)));
   CHECK(queue.isServiceEvent(event(EventType::Out, usb::cdcDataOutEndpoint)));
   CHECK_FALSE(queue.isServiceEvent(event(EventType::InComplete, 0x83)));
   CHECK_FALSE(queue.isServiceEvent(event(EventType::Setup)));
   CHECK_FALSE(queue.isServiceEvent(event(EventType::Reset)));

   REQUIRE(queue.try_push(event(EventType::InComplete, 0x82)));
   REQUIRE(queue.try_push(event(EventType::Out, 0x03)));
   REQUIRE(queue.try_push(event(EventType::Out, usb::cdcDataOutEndpoint)));
   REQUIRE(queue.try_push(event(EventType::Setup)));
   REQUIRE(queue.try_push(event(EventType::Out, 0x03)));
   queue.setSerialOnly(true);
   const auto service = popAll(queue);
   REQUIRE(service.size() == 3);
   CHECK(service[0].endpoint == 0x82);
   CHECK(service[1].endpoint == 0x03);
   CHECK(service[2].endpoint == usb::cdcDataOutEndpoint);
   // The SETUP and the packet after it wait.
   CHECK(queue.size() == 2);
}

TEST_CASE("SerialEventQueue: without command endpoints, the serial-only mode stops at a protocol event") {
   Queue queue;
   CHECK_FALSE(queue.isServiceEvent(event(EventType::Out, usb::versaloonOutEndpoint)));
   REQUIRE(queue.try_push(event(EventType::Out, usb::versaloonOutEndpoint)));
   REQUIRE(queue.try_push(event(EventType::Out, usb::cdcDataOutEndpoint)));
   queue.setSerialOnly(true);
   CHECK(popAll(queue).empty());
   CHECK(queue.size() == 2);
}

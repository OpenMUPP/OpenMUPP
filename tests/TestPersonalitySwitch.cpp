#include "personality/PersonalitySwitch.h"
#include "protocol/CommandProcessor.h"
#include "protocol/Versaloon.h"

#include "CmsisDapBench.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using personality::Personality;
using personality::PersonalitySwitch;
using vsltest::DapBench;
using Bytes = std::vector<uint8_t>;

namespace {

const protocol::DeviceInfo versaloonInfo{ .version = "v1.2.3", .board = "mini", .hardwareId = 0x01 };

// Runs one Versaloon command. No value for no reply.
std::optional<Bytes> runVersaloon(protocol::CommandProcessor &processor, const Bytes &command) {
   std::array<uint8_t, protocol::bufferSize> reply{};
   const auto size = processor.process(art::span<const uint8_t>(command.data(), command.size()),
                                       art::span<uint8_t>(reply.data(), reply.size()));
   if(!size) {
      return std::nullopt;
   }
   return Bytes(reply.begin(), reply.begin() + static_cast<std::ptrdiff_t>(*size));
}

} // namespace

TEST_CASE("The personality switch is not due without a request") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };

   CHECK(personalitySwitch.active() == Personality::Versaloon);
   CHECK_FALSE(personalitySwitch.requested().has_value());
   CHECK_FALSE(personalitySwitch.isDue(0));
   CHECK_FALSE(personalitySwitch.isDue(1'000'000));
}

TEST_CASE("The personality switch is due after the reply delay") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   constexpr uint64_t start = 5'000'000'000'000;

   personalitySwitch.request(Personality::CmsisDap);
   CHECK(personalitySwitch.requested() == Personality::CmsisDap);
   CHECK_FALSE(personalitySwitch.isDue(start));
   CHECK_FALSE(personalitySwitch.isDue(start + PersonalitySwitch::replyDelayMs - 1));
   CHECK(personalitySwitch.isDue(start + PersonalitySwitch::replyDelayMs));
}

TEST_CASE("A request for the active personality does nothing") {
   PersonalitySwitch personalitySwitch{ Personality::CmsisDap };

   personalitySwitch.request(Personality::CmsisDap);
   CHECK_FALSE(personalitySwitch.requested().has_value());
   CHECK_FALSE(personalitySwitch.isDue(1'000));
}

TEST_CASE("A second request does not move the time") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };

   personalitySwitch.request(Personality::CmsisDap);
   CHECK_FALSE(personalitySwitch.isDue(100));
   personalitySwitch.request(Personality::CmsisDap);
   CHECK_FALSE(personalitySwitch.isDue(105));
   CHECK(personalitySwitch.isDue(100 + PersonalitySwitch::replyDelayMs));
}

TEST_CASE("A request for the active personality cancels a pending request") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };

   personalitySwitch.request(Personality::CmsisDap);
   CHECK_FALSE(personalitySwitch.isDue(100));
   personalitySwitch.request(Personality::Versaloon);
   CHECK_FALSE(personalitySwitch.requested().has_value());
   CHECK_FALSE(personalitySwitch.isDue(100 + PersonalitySwitch::replyDelayMs));

   // A new request gets a new time
   personalitySwitch.request(Personality::CmsisDap);
   CHECK_FALSE(personalitySwitch.isDue(200));
   CHECK(personalitySwitch.isDue(200 + PersonalitySwitch::replyDelayMs));
}

TEST_CASE("SWITCH_PERSONALITY requests the personality") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   protocol::CommandProcessor processor(versaloonInfo, nullptr, nullptr, &personalitySwitch);

   CHECK(protocol::switchPersonality == 0x0E);
   CHECK(runVersaloon(processor, { protocol::switchPersonality, 0x01 }) == Bytes{ protocol::switchAccepted });
   CHECK(personalitySwitch.requested() == Personality::CmsisDap);
}

TEST_CASE("SWITCH_PERSONALITY to the active personality is accepted and does nothing") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   protocol::CommandProcessor processor(versaloonInfo, nullptr, nullptr, &personalitySwitch);

   CHECK(runVersaloon(processor, { protocol::switchPersonality, 0x00 }) == Bytes{ protocol::switchAccepted });
   CHECK_FALSE(personalitySwitch.requested().has_value());
}

TEST_CASE("SWITCH_PERSONALITY refuses a code that is not a personality") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   protocol::CommandProcessor processor(versaloonInfo, nullptr, nullptr, &personalitySwitch);

   CHECK(runVersaloon(processor, { protocol::switchPersonality, 0x02 }) == Bytes{ protocol::switchRefused });
   CHECK(runVersaloon(processor, { protocol::switchPersonality }) == Bytes{ protocol::switchRefused });
   CHECK_FALSE(personalitySwitch.requested().has_value());
}

TEST_CASE("SWITCH_PERSONALITY is refused in an image with one personality") {
   protocol::CommandProcessor processor(versaloonInfo, nullptr, nullptr);

   CHECK(runVersaloon(processor, { protocol::switchPersonality, 0x01 }) == Bytes{ protocol::switchRefused });
}

TEST_CASE("ID_DAP_Vendor0 requests the personality after its response") {
   PersonalitySwitch personalitySwitch{ Personality::CmsisDap };
   DapBench bench{ "Product", &personalitySwitch };
   bench.enumerate();

   REQUIRE(bench.write({ cmsisdap::command::vendor0, 0x00 }));
   CHECK(bench.read() == Bytes{ cmsisdap::command::vendor0, 0x00 });
   CHECK(personalitySwitch.requested() == Personality::Versaloon);
}

TEST_CASE("ID_DAP_Vendor0 refuses a code that is not a personality") {
   PersonalitySwitch personalitySwitch{ Personality::CmsisDap };
   DapBench bench{ "Product", &personalitySwitch };
   bench.enumerate();

   REQUIRE(bench.write({ cmsisdap::command::vendor0, 0x07 }));
   CHECK(bench.read() == Bytes{ cmsisdap::command::vendor0, 0x01 });
   // A short request
   REQUIRE(bench.write({ cmsisdap::command::vendor0 }));
   CHECK(bench.read() == Bytes{ cmsisdap::command::invalid });
   CHECK_FALSE(personalitySwitch.requested().has_value());
}

TEST_CASE("ID_DAP_Vendor0 is invalid in an image with one personality") {
   DapBench bench;
   bench.enumerate();

   REQUIRE(bench.write({ cmsisdap::command::vendor0, 0x00 }));
   CHECK(bench.read() == Bytes{ cmsisdap::command::invalid });
}

TEST_CASE("The key gesture delay gives three flashes of all LEDs") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   personalitySwitch.request(Personality::CmsisDap, PersonalitySwitch::flashDelayMs);

   CHECK_FALSE(personalitySwitch.isDue(1000));
   unsigned flashes = 0;
   bool wasOn = false;
   for(uint64_t t = 1000; t < 1000 + PersonalitySwitch::flashDelayMs; ++t) {
      const auto flash = personalitySwitch.indication(t);
      REQUIRE(flash.has_value());
      if(*flash && !wasOn) {
         ++flashes;
      }
      wasOn = *flash;
   }
   CHECK(flashes == 3);
   CHECK_FALSE(personalitySwitch.isDue(1000 + PersonalitySwitch::flashDelayMs - 1));
   CHECK(personalitySwitch.isDue(1000 + PersonalitySwitch::flashDelayMs));
   CHECK_FALSE(personalitySwitch.indication(1000 + PersonalitySwitch::flashDelayMs).has_value());
}

TEST_CASE("A switch command gives no LED indication") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };

   CHECK_FALSE(personalitySwitch.indication(0).has_value());
   personalitySwitch.request(Personality::CmsisDap);
   CHECK_FALSE(personalitySwitch.isDue(0));
   CHECK_FALSE(personalitySwitch.indication(1).has_value());
   CHECK(personalitySwitch.isDue(PersonalitySwitch::replyDelayMs));
}

TEST_CASE("The reset waits for the release of the key") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   personalitySwitch.request(Personality::CmsisDap, PersonalitySwitch::flashDelayMs);
   constexpr uint64_t due = 1000 + PersonalitySwitch::flashDelayMs;

   CHECK_FALSE(personalitySwitch.isDue(1000, false));
   // The key is pressed again in the flash window and stays down
   CHECK_FALSE(personalitySwitch.isDue(due, true));
   CHECK_FALSE(personalitySwitch.isDue(due + 5000, true));
   CHECK(personalitySwitch.isDue(due + 5001, false));
}

#include "protocol/SwdBlock.h"

namespace protocol {

namespace {

constexpr std::size_t bitCountSize = 2;
constexpr std::size_t configSize = 5;
constexpr std::size_t transferSize = 5;

// The data bytes of a reply. The host expects them also when the
// sub-command fails. 0 for an IN without a bit count.
std::size_t dataSize(const SubCommand &command) {
   switch(command.command) {
   case subcommand::in:
      return command.payload.size() < bitCountSize ? 0 : swd::byteCount(getU16(command.payload, 0));
   case subcommand::inOut:
      return transferSize;
   default:
      return 0;
   }
}

void putU32(art::span<uint8_t> data, std::size_t offset, uint32_t value) {
   for(std::size_t i = 0; i < 4; ++i) {
      data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
   }
}

} // namespace

SwdBlock::SwdBlock(SwdPort &port)
   : SubCommandBlock(block::swd)
   , port_(port) {
}

void SwdBlock::runSubCommand(const SubCommand &command, Reply &reply) {
   auto slot = reply.reserve(dataSize(command));
   if(!slot) {
      return;
   }
   if(command.index != 0) {
      slot->setStatus(Status::InvalidIndex);
      return;
   }
   const auto &payload = command.payload;
   switch(command.command) {
   case subcommand::init:
      port_.init();
      port_.configure(swd::defaultTurnaround, swd::defaultRetries, swd::defaultDelay);
      return;
   case subcommand::fini:
      port_.fini();
      return;
   case subcommand::config:
      slot->setStatus(config(payload));
      return;
   case subcommand::out:
      slot->setStatus(sequenceOut(payload));
      return;
   case subcommand::in:
      if(payload.size() != bitCountSize) {
         slot->setStatus(Status::InvalidPara);
         return;
      }
      port_.sequenceIn(getU16(payload, 0), slot->data());
      return;
   case subcommand::inOut: {
      if(payload.size() != transferSize) {
         slot->setStatus(Status::InvalidPara);
         return;
      }
      uint32_t data = getU32(payload, 1);
      const uint8_t ack = port_.transfer(swd::encodeRequest(payload[0]), data);
      slot->data()[0] = ack;
      putU32(slot->data(), 1, data);
      const Status status = swd::statusOf(ack);
      if(status != Status::Ok) {
         ++errorCount_;
      }
      slot->setStatus(status);
      return;
   }
   default:
      slot->setStatus(Status::CmdNotSupport);
      return;
   }
}

Status SwdBlock::config(art::span<const uint8_t> payload) {
   if(payload.size() != configSize) {
      return Status::InvalidPara;
   }
   // Both turnarounds of a transfer together, see SwdBlock.h
   const uint8_t trn = payload[0];
   if(trn < 2 || trn > 8 || trn % 2 != 0) {
      return Status::InvalidPara;
   }
   port_.configure(static_cast<uint8_t>(trn / 2), getU16(payload, 1), getU16(payload, 3));
   return Status::Ok;
}

Status SwdBlock::sequenceOut(art::span<const uint8_t> payload) {
   if(payload.size() < bitCountSize) {
      return Status::InvalidPara;
   }
   const uint16_t bitCount = getU16(payload, 0);
   if(payload.size() - bitCountSize != swd::byteCount(bitCount)) {
      return Status::InvalidPara;
   }
   port_.sequenceOut(bitCount, payload.subspan(bitCountSize));
   return Status::Ok;
}

} // namespace protocol

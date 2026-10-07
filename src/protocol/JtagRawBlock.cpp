#include "protocol/JtagRawBlock.h"

namespace protocol {

namespace {

constexpr std::size_t bitCountSize = 4;

// The TDO bytes of an IN_OUT reply. The host expects them also when the
// sub-command fails. 0 when the payload has no bit count.
std::size_t tdoSize(const SubCommand &command) {
   if(command.command != subcommand::inOut || command.payload.size() < bitCountSize) {
      return 0;
   }
   return jtagRaw::byteCount(getU32(command.payload, 0));
}

} // namespace

JtagRawBlock::JtagRawBlock(JtagPort &port)
   : SubCommandBlock(block::jtagRaw)
   , port_(port) {
}

void JtagRawBlock::runSubCommand(const SubCommand &command, Reply &reply) {
   const std::size_t bytes = tdoSize(command);
   auto slot = reply.reserve(bytes);
   if(!slot) {
      return;
   }
   if(command.index != 0) {
      slot->setStatus(Status::InvalidIndex);
      return;
   }
   switch(command.command) {
   case subcommand::init:
      port_.init();
      static_cast<void>(port_.setSpeed(jtagRaw::defaultKHz));
      return;
   case subcommand::fini:
      port_.fini();
      return;
   case subcommand::config:
      if(command.payload.size() != 4 || !port_.setSpeed(getU32(command.payload, 0))) {
         slot->setStatus(Status::InvalidPara);
      }
      return;
   case subcommand::inOut: {
      const auto &payload = command.payload;
      // bytes is at most 2^29 + 1, so 2 * bytes does not overflow.
      if(payload.size() < bitCountSize || payload.size() - bitCountSize != 2 * bytes) {
         slot->setStatus(Status::InvalidPara);
         return;
      }
      if(!port_.shift(getU32(payload, 0), payload.subspan(bitCountSize, bytes),
                      payload.subspan(bitCountSize + bytes, bytes), slot->data())) {
         slot->setStatus(Status::Timeout);
      }
      return;
   }
   default:
      slot->setStatus(Status::CmdNotSupport);
      return;
   }
}

} // namespace protocol

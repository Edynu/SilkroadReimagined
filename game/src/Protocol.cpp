#include "sr/game/Protocol.h"

namespace sr::game::proto {

Welcome AnswerHello(const Hello& hello, std::string_view serverName) {
    Welcome welcome;
    welcome.server = std::string(serverName);
    if (hello.protocol != kProtocolVersion) {
        welcome.result = WelcomeResult::ProtocolMismatch;
        welcome.reason = std::format("this server speaks protocol {}, the client {}", kProtocolVersion, hello.protocol);
    }
    return welcome;
}

} // namespace sr::game::proto

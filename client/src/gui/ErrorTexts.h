#pragma once

// The original's words for a server's refusal code: the 1.188 client's one table from a code (0x0002 .. 0x5xxx: the
// item and storage codes 0x18xx, the guild's 0x4Cxx, the quest's 0x30xx, ...) to its text key, whatever message carried
// it. Ours sends the original's codes in most refusals; a window with words of its own for a code keeps them, and this
// is the rest's (the item use's refusals, the fallbacks of the windows' own tables).

#include <cstdint>

namespace sr::client {

// The code's text key; null when the original has none for it.
const char* OriginalErrorKey(std::uint16_t code);

} // namespace sr::client

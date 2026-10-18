// NovaChangJie - messages between the text service and the NovaChangJieLM
// helper (named pipe kLmPipePrefix + user SID, message mode, little endian).
//
//   request:  uint32 kLmMagic, uint32 count, then count x (uint32 length,
//             length x UTF-16 code units)
//   reply:    uint32 kLmMagic, uint32 count, count x float32 log P(sentence)
//
// An empty reply (count 0) means the helper has no model.
#pragma once

#include <cstdint>

namespace nova {

inline constexpr uint32_t kLmMagic = 0x314D4C4E;  // "NLM1"
inline constexpr uint32_t kLmMaxSentences = 16;
inline constexpr uint32_t kLmMaxLength = 64;  // UTF-16 code units per sentence.
inline constexpr uint32_t kLmMaxRequest = 8 + kLmMaxSentences * (4 + kLmMaxLength * 2);
inline constexpr uint32_t kLmMaxReply = 8 + kLmMaxSentences * 4;

}  // namespace nova

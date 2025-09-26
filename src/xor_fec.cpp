#include "xor_fec.h"

#include <algorithm>

namespace ve {

FecPacket xor_parity(const std::vector<std::vector<std::uint8_t>>& packets,
                     std::uint32_t group_id) {
  FecPacket fec;
  fec.group_id = group_id;
  fec.count = static_cast<std::uint16_t>(packets.size());
  size_t maxlen = 0;
  for (const auto& p : packets) maxlen = std::max(maxlen, p.size());
  fec.data.assign(maxlen, 0);
  for (const auto& p : packets) {
    for (size_t i = 0; i < p.size(); ++i) fec.data[i] ^= p[i];
  }
  return fec;
}

std::vector<std::uint8_t> xor_recover(const std::vector<std::vector<std::uint8_t>>& received,
                                      const FecPacket& parity) {
  // Only works if exactly one packet is missing and the rest align to max len.
  size_t maxlen = parity.data.size();
  std::vector<std::uint8_t> out = parity.data; // start from parity, XOR back
  for (const auto& p : received) {
    for (size_t i = 0; i < p.size(); ++i) out[i] ^= p[i];
  }
  return out;
}

}  // namespace ve

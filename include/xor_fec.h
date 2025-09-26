// Simple XOR FEC stubs (not integrated in pipeline; using ULPFEC in GStreamer)
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ve {

struct FecPacket {
  std::vector<std::uint8_t> data;
  std::uint32_t group_id = 0;
  std::uint16_t count = 0; // number of packets XORed
};

// Produces a simple parity packet (XOR of N packets) for demonstration.
FecPacket xor_parity(const std::vector<std::vector<std::uint8_t>>& packets,
                     std::uint32_t group_id);

// Attempt to recover one missing packet by XORing the rest with parity.
// Returns empty vector on failure.
std::vector<std::uint8_t> xor_recover(const std::vector<std::vector<std::uint8_t>>& received,
                                      const FecPacket& parity);

}  // namespace ve

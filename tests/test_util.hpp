#pragma once

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "poker/eval/cards.hpp"

namespace poker::test {

// Parses a space-separated hand like "As Kd Qh Jc Ts".
inline std::vector<eval::Card> hand(std::string_view s) {
  std::vector<eval::Card> cards;
  std::istringstream in{std::string{s}};
  std::string tok;
  while (in >> tok) {
    const auto c = eval::parse_card(tok);
    if (!c) {
      throw std::invalid_argument("bad card: " + tok);
    }
    cards.push_back(*c);
  }
  return cards;
}

inline std::string hand_str(const std::vector<eval::Card>& cards) {
  std::string out;
  for (const auto c : cards) {
    if (!out.empty()) {
      out += ' ';
    }
    out += eval::card_str(c).data();
  }
  return out;
}

// SplitMix64: tiny, seedable, reproducible across platforms.
class Rng {
public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  // Lemire's unbiased bounded draw.
  std::uint32_t below(std::uint32_t bound) {
    std::uint64_t product = (next() >> 32) * static_cast<std::uint64_t>(bound);
    return static_cast<std::uint32_t>(product >> 32);
  }

private:
  std::uint64_t state_;
};

} // namespace poker::test

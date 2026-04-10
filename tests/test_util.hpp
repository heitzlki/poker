#pragma once

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "poker/core/shuffle.hpp"
#include "poker/eval/cards.hpp"

namespace poker::test {

using Rng = core::Rng;

inline std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::strtoull(v, nullptr, 10) : fallback;
}

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

} // namespace poker::test

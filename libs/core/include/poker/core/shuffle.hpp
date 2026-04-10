#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "poker/eval/cards.hpp"

namespace poker::core {

// SplitMix64. Small, seedable, and identical on every platform, which is why
// it is used instead of <random> (see docs/rules.md on randomness).
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
    const std::uint64_t product = (next() >> 32) * static_cast<std::uint64_t>(bound);
    return static_cast<std::uint32_t>(product >> 32);
  }

private:
  std::uint64_t state_;
};

// Fisher-Yates over the given cards.
inline void shuffle(std::span<eval::Card> cards, Rng& rng) {
  const auto n = static_cast<std::uint32_t>(cards.size());
  for (std::uint32_t i = 0; i + 1 < n; ++i) {
    const std::uint32_t j = i + rng.below(n - i);
    const eval::Card tmp = cards[i];
    cards[i] = cards[j];
    cards[j] = tmp;
  }
}

inline std::array<eval::Card, eval::kNumCards> shuffled_deck(Rng& rng) {
  std::array<eval::Card, eval::kNumCards> deck{};
  for (int i = 0; i < eval::kNumCards; ++i) {
    deck[static_cast<std::size_t>(i)] = static_cast<eval::Card>(i);
  }
  shuffle(deck, rng);
  return deck;
}

inline std::array<eval::Card, eval::kNumCards> shuffled_deck(std::uint64_t seed) {
  Rng rng{seed};
  return shuffled_deck(rng);
}

} // namespace poker::core

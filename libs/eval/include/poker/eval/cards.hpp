#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace poker::eval {

// A card is an integer in [0, 52): card = rank * 4 + suit.
// Ranks: 0 = deuce .. 12 = ace. Suits: 0 = clubs, 1 = diamonds, 2 = hearts, 3 = spades.
using Card = std::uint8_t;

// Bit i set <=> card i present.
using CardSet = std::uint64_t;

inline constexpr int kNumRanks = 13;
inline constexpr int kNumSuits = 4;
inline constexpr int kNumCards = 52;

constexpr int rank_of(Card c) {
  return c >> 2;
}
constexpr int suit_of(Card c) {
  return c & 3;
}
constexpr Card make_card(int rank, int suit) {
  return static_cast<Card>(rank * 4 + suit);
}
constexpr CardSet card_bit(Card c) {
  return CardSet{1} << c;
}

inline constexpr std::string_view kRankChars = "23456789TJQKA";
inline constexpr std::string_view kSuitChars = "cdhs";

constexpr std::optional<Card> parse_card(std::string_view s) {
  if (s.size() != 2) {
    return std::nullopt;
  }
  const auto rank = kRankChars.find(s[0]);
  const auto suit = kSuitChars.find(s[1]);
  if (rank == std::string_view::npos || suit == std::string_view::npos) {
    return std::nullopt;
  }
  return make_card(static_cast<int>(rank), static_cast<int>(suit));
}

// Null-terminated two-character representation, e.g. "As".
constexpr std::array<char, 3> card_str(Card c) {
  return {kRankChars[static_cast<std::size_t>(rank_of(c))],
          kSuitChars[static_cast<std::size_t>(suit_of(c))], '\0'};
}

} // namespace poker::eval

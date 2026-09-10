#pragma once

#include <cstdint>
#include <string_view>

namespace poker::eval {

// Totally ordered rank of a 5-card high hand: greater value beats lower value.
//
// Bit layout: [23:20] category, [19:16] t0, [15:12] t1, [11:8] t2, [7:4] t3, [3:0] t4.
// t0..t4 are category-specific tiebreak ranks (0 = deuce .. 12 = ace), listed from most
// to least significant; slots a category does not use are 0. For straights t0 is the
// high card of the run, with the wheel (A-5) encoded as t0 = 3 (the five).
using HandRank = std::uint32_t;

enum class HandCategory : std::uint8_t {
  kHighCard = 0,
  kOnePair = 1,
  kTwoPair = 2,
  kTrips = 3,
  kStraight = 4,
  kFlush = 5,
  kFullHouse = 6,
  kQuads = 7,
  kStraightFlush = 8,
};

constexpr HandRank make_rank(HandCategory cat, int t0 = 0, int t1 = 0, int t2 = 0, int t3 = 0,
                             int t4 = 0) {
  return static_cast<HandRank>(cat) << 20 | static_cast<HandRank>(t0) << 16 |
         static_cast<HandRank>(t1) << 12 | static_cast<HandRank>(t2) << 8 |
         static_cast<HandRank>(t3) << 4 | static_cast<HandRank>(t4);
}

constexpr HandCategory category(HandRank r) {
  return static_cast<HandCategory>(r >> 20);
}

constexpr std::string_view category_name(HandCategory cat) {
  switch (cat) {
  case HandCategory::kHighCard:
    return "high card";
  case HandCategory::kOnePair:
    return "one pair";
  case HandCategory::kTwoPair:
    return "two pair";
  case HandCategory::kTrips:
    return "three of a kind";
  case HandCategory::kStraight:
    return "straight";
  case HandCategory::kFlush:
    return "flush";
  case HandCategory::kFullHouse:
    return "full house";
  case HandCategory::kQuads:
    return "four of a kind";
  case HandCategory::kStraightFlush:
    return "straight flush";
  }
  return "invalid";
}

} // namespace poker::eval

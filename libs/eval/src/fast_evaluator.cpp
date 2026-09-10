#include "poker/eval/evaluator.hpp"

#include <bit>
#include <cassert>
#include <cstdint>

namespace poker::eval {

namespace {

// Highest straight in a 13-bit rank mask, or -1. The wheel maps to 3 (five-high).
constexpr int straight_high(std::uint32_t rank_mask) {
  for (int high = 12; high >= 4; --high) {
    const std::uint32_t run = 0x1Fu << (high - 4);
    if ((rank_mask & run) == run) {
      return high;
    }
  }
  constexpr std::uint32_t kWheel = 0x100Fu; // A,5,4,3,2
  if ((rank_mask & kWheel) == kWheel) {
    return 3;
  }
  return -1;
}

// Ranks of the top `n` set bits, most significant first, packed as tiebreak nibbles.
constexpr std::uint32_t top_bits(std::uint32_t rank_mask, int n) {
  std::uint32_t out = 0;
  int taken = 0;
  for (int r = 12; r >= 0 && taken < n; --r) {
    if ((rank_mask >> r) & 1u) {
      out |= static_cast<std::uint32_t>(r) << (4 * (4 - taken));
      ++taken;
    }
  }
  return out;
}

} // namespace

HandRank evaluate_fast(std::span<const Card> cards) {
  assert(cards.size() >= 5 && cards.size() <= 7);

  std::uint32_t suit_mask[kNumSuits] = {};
  std::uint8_t count[kNumRanks] = {};
  std::uint32_t rank_mask = 0;
  for (const Card c : cards) {
    const auto r = static_cast<std::uint32_t>(rank_of(c));
    suit_mask[suit_of(c)] |= 1u << r;
    ++count[r];
    rank_mask |= 1u << r;
  }

  int flush_suit = -1;
  for (int s = 0; s < kNumSuits; ++s) {
    if (std::popcount(suit_mask[s]) >= 5) {
      flush_suit = s;
      break; // at most one suit can hold 5+ of 7 cards
    }
  }

  if (flush_suit >= 0) {
    const int sf_high = straight_high(suit_mask[flush_suit]);
    if (sf_high >= 0) {
      return make_rank(HandCategory::kStraightFlush, sf_high);
    }
  }

  int quad = -1, trips = -1, second_trips = -1, pair_hi = -1, pair_lo = -1, pair_third = -1;
  for (int r = 12; r >= 0; --r) {
    switch (count[r]) {
    case 4:
      quad = r;
      break;
    case 3:
      (trips < 0 ? trips : second_trips) = r;
      break;
    case 2:
      (pair_hi < 0 ? pair_hi : (pair_lo < 0 ? pair_lo : pair_third)) = r;
      break;
    default:
      break;
    }
  }

  if (quad >= 0) {
    const std::uint32_t kickers = rank_mask & ~(1u << quad);
    return make_rank(HandCategory::kQuads, quad) | top_bits(kickers, 1) >> 4;
  }

  if (trips >= 0 && (second_trips >= 0 || pair_hi >= 0)) {
    const int pair = second_trips >= 0 && second_trips > pair_hi ? second_trips
                     : pair_hi >= 0                              ? pair_hi
                                                                 : second_trips;
    return make_rank(HandCategory::kFullHouse, trips, pair);
  }

  if (flush_suit >= 0) {
    return make_rank(HandCategory::kFlush) | top_bits(suit_mask[flush_suit], 5);
  }

  const int st_high = straight_high(rank_mask);
  if (st_high >= 0) {
    return make_rank(HandCategory::kStraight, st_high);
  }

  if (trips >= 0) {
    const std::uint32_t kickers = rank_mask & ~(1u << trips);
    return make_rank(HandCategory::kTrips, trips) | top_bits(kickers, 2) >> 4;
  }

  if (pair_hi >= 0 && pair_lo >= 0) {
    const std::uint32_t kickers = rank_mask & ~(1u << pair_hi) & ~(1u << pair_lo);
    return make_rank(HandCategory::kTwoPair, pair_hi, pair_lo) | top_bits(kickers, 1) >> 8;
  }

  if (pair_hi >= 0) {
    const std::uint32_t kickers = rank_mask & ~(1u << pair_hi);
    return make_rank(HandCategory::kOnePair, pair_hi) | top_bits(kickers, 3) >> 4;
  }

  return make_rank(HandCategory::kHighCard) | top_bits(rank_mask, 5);
}

} // namespace poker::eval

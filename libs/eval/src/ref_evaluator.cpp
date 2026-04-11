#include "poker/eval/evaluator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <functional>

namespace poker::eval {

namespace {

HandRank rank_five(std::array<Card, 5> hand) {
  std::array<int, 5> ranks{};
  for (std::size_t i = 0; i < 5; ++i) {
    ranks[i] = rank_of(hand[i]);
  }
  std::sort(ranks.begin(), ranks.end(), std::greater<>());

  const bool flush =
      std::all_of(hand.begin(), hand.end(), [&](Card c) { return suit_of(c) == suit_of(hand[0]); });

  const bool distinct = std::adjacent_find(ranks.begin(), ranks.end()) == ranks.end();
  bool straight = false;
  int straight_high = 0;
  if (distinct && ranks[0] - ranks[4] == 4) {
    straight = true;
    straight_high = ranks[0];
  } else if (distinct && ranks == std::array<int, 5>{12, 3, 2, 1, 0}) {
    straight = true; // the wheel: A-5-4-3-2 plays as a five-high straight
    straight_high = 3;
  }

  if (straight && flush) {
    return make_rank(HandCategory::kStraightFlush, straight_high);
  }

  // Group equal ranks, ordered by (group size desc, rank desc); ranks[] is already
  // sorted desc, so groups come out in classification order.
  struct Group {
    int count = 0;
    int rank = 0;
  };
  std::array<Group, 5> groups{};
  int n_groups = 0;
  for (const int r : ranks) {
    if (n_groups > 0 && groups[static_cast<std::size_t>(n_groups - 1)].rank == r) {
      ++groups[static_cast<std::size_t>(n_groups - 1)].count;
    } else {
      groups[static_cast<std::size_t>(n_groups++)] = {1, r};
    }
  }
  std::stable_sort(groups.begin(), groups.begin() + n_groups,
                   [](const Group& a, const Group& b) { return a.count > b.count; });
  const auto& g = groups;

  if (g[0].count == 4) {
    return make_rank(HandCategory::kQuads, g[0].rank, g[1].rank);
  }
  if (g[0].count == 3 && g[1].count == 2) {
    return make_rank(HandCategory::kFullHouse, g[0].rank, g[1].rank);
  }
  if (flush) {
    return make_rank(HandCategory::kFlush, ranks[0], ranks[1], ranks[2], ranks[3], ranks[4]);
  }
  if (straight) {
    return make_rank(HandCategory::kStraight, straight_high);
  }
  if (g[0].count == 3) {
    return make_rank(HandCategory::kTrips, g[0].rank, g[1].rank, g[2].rank);
  }
  if (g[0].count == 2 && g[1].count == 2) {
    return make_rank(HandCategory::kTwoPair, g[0].rank, g[1].rank, g[2].rank);
  }
  if (g[0].count == 2) {
    return make_rank(HandCategory::kOnePair, g[0].rank, g[1].rank, g[2].rank, g[3].rank);
  }
  return make_rank(HandCategory::kHighCard, ranks[0], ranks[1], ranks[2], ranks[3], ranks[4]);
}

} // namespace

HandRank evaluate_ref(std::span<const Card> cards) {
  const auto n = cards.size();
  assert(n >= 5 && n <= 7);

  HandRank best = 0;
  for (unsigned mask = 0; mask < (1u << n); ++mask) {
    if (std::popcount(mask) != 5) {
      continue;
    }
    std::array<Card, 5> five{};
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if ((mask >> i) & 1u) {
        five[k++] = cards[i];
      }
    }
    best = std::max(best, rank_five(five));
  }
  return best;
}

} // namespace poker::eval

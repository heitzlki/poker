#include <doctest/doctest.h>

#include <array>
#include <map>

#include "poker/eval/evaluator.hpp"
#include "test_util.hpp"

using namespace poker;
using namespace poker::eval;

// All C(52,5) = 2,598,960 five-card hands: the fast evaluator must agree with the
// reference bit for bit, and the reference itself must reproduce the known category
// frequencies — an oracle that is independent of both implementations.
TEST_CASE("exhaustive five-card differential and category census") {
  std::map<HandCategory, long> census;
  long total = 0;

  std::array<Card, 5> cards{};
  for (Card a = 0; a < 48; ++a) {
    cards[0] = a;
    for (Card b = a + 1; b < 49; ++b) {
      cards[1] = b;
      for (Card c = b + 1; c < 50; ++c) {
        cards[2] = c;
        for (Card d = c + 1; d < 51; ++d) {
          cards[3] = d;
          for (Card e = d + 1; e < 52; ++e) {
            cards[4] = e;
            const HandRank ref = evaluate_ref(cards);
            const HandRank fast = evaluate_fast(cards);
            if (ref != fast) {
              CAPTURE(test::hand_str({cards.begin(), cards.end()}));
              REQUIRE(ref == fast);
            }
            ++census[category(ref)];
            ++total;
          }
        }
      }
    }
  }

  CHECK(total == 2598960);
  CHECK(census[HandCategory::kStraightFlush] == 40);
  CHECK(census[HandCategory::kQuads] == 624);
  CHECK(census[HandCategory::kFullHouse] == 3744);
  CHECK(census[HandCategory::kFlush] == 5108);
  CHECK(census[HandCategory::kStraight] == 10200);
  CHECK(census[HandCategory::kTrips] == 54912);
  CHECK(census[HandCategory::kTwoPair] == 123552);
  CHECK(census[HandCategory::kOnePair] == 1098240);
  CHECK(census[HandCategory::kHighCard] == 1302540);
}

// Random 7-card hands, reproducible from POKER_DIFF_SEED; iteration count is
// tunable via POKER_DIFF_ITERS for longer local or nightly runs.
TEST_CASE("random seven-card differential") {
  const std::uint64_t seed = test::env_u64("POKER_DIFF_SEED", 20260909);
  const std::uint64_t iters = test::env_u64("POKER_DIFF_ITERS", 200000);
  test::Rng rng{seed};

  std::array<Card, kNumCards> deck{};
  for (int i = 0; i < kNumCards; ++i) {
    deck[static_cast<std::size_t>(i)] = static_cast<Card>(i);
  }

  for (std::uint64_t it = 0; it < iters; ++it) {
    // Partial Fisher-Yates: the first 7 slots become a uniform 7-card draw.
    for (std::uint32_t i = 0; i < 7; ++i) {
      const std::uint32_t j = i + rng.below(static_cast<std::uint32_t>(kNumCards) - i);
      std::swap(deck[i], deck[j]);
    }
    const std::span<const Card> hand{deck.data(), 7};

    const HandRank ref = evaluate_ref(hand);
    const HandRank fast = evaluate_fast(hand);
    if (ref != fast) {
      CAPTURE(seed);
      CAPTURE(it);
      CAPTURE(test::hand_str({hand.begin(), hand.end()}));
      REQUIRE(ref == fast);
    }
  }
}

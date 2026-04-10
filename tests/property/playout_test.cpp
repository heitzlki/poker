#include <doctest/doctest.h>

#include <vector>

#include "poker/core/engine.hpp"
#include "poker/core/shuffle.hpp"
#include "test_util.hpp"

using namespace poker;
using namespace poker::core;

namespace {

Action pick_action(const LegalActions& la, test::Rng& rng) {
  // Weighted toward continuing so hands reach showdown often enough.
  Action kinds[4];
  std::uint32_t n = 0;
  kinds[n++] = {la.can_check ? Action::Kind::kCheck : Action::Kind::kCall, 0};
  kinds[n++] = kinds[0];
  if (la.can_raise) {
    kinds[n++] = {Action::Kind::kRaise, 0};
  }
  kinds[n++] = {Action::Kind::kFold, 0};

  Action a = kinds[rng.below(n)];
  if (a.kind == Action::Kind::kRaise) {
    switch (rng.below(3)) {
    case 0:
      a.amount = la.min_raise_to;
      break;
    case 1:
      a.amount = la.max_raise_to;
      break;
    default:
      a.amount = la.min_raise_to + static_cast<Chips>(rng.below(static_cast<std::uint32_t>(
                                       la.max_raise_to - la.min_raise_to + 1)));
      break;
    }
  }
  return a;
}

} // namespace

// Random hands with random tables and stacks, driven only through
// legal_actions. Chips must be conserved after every action and the payout
// must hand the whole pot back.
TEST_CASE("random playouts conserve chips") {
  const std::uint64_t seed = test::env_u64("POKER_PLAYOUT_SEED", 20260910);
  const std::uint64_t hands = test::env_u64("POKER_PLAYOUT_HANDS", 3000);
  test::Rng rng{seed};

  for (std::uint64_t it = 0; it < hands; ++it) {
    CAPTURE(seed);
    CAPTURE(it);

    const int n = 2 + static_cast<int>(rng.below(kMaxSeats - 1));
    const int button = static_cast<int>(rng.below(static_cast<std::uint32_t>(n)));
    std::vector<Chips> stacks(static_cast<std::size_t>(n));
    Chips total = 0;
    for (auto& stack : stacks) {
      stack = 1 + static_cast<Chips>(rng.below(300)); // small stacks force all-ins
      total += stack;
    }
    const TableConfig cfg{5, 10};
    const auto deck = shuffled_deck(rng);

    State s = new_hand(cfg, stacks, button, deck);
    int steps = 0;
    while (!is_terminal(s)) {
      REQUIRE(++steps < 500);
      const LegalActions la = legal_actions(s);
      REQUIRE(la.can_fold);
      REQUIRE(la.can_check != la.can_call);
      if (la.can_raise) {
        REQUIRE(la.min_raise_to <= la.max_raise_to);
        REQUIRE(la.max_raise_to > s.current_bet);
      }

      apply(s, pick_action(la, rng));

      Chips held = 0;
      for (int i = 0; i < n; ++i) {
        REQUIRE(s.stacks[i] >= 0);
        REQUIRE(s.street_committed[i] <= s.total_committed[i]);
        held += s.stacks[i] + s.total_committed[i];
      }
      REQUIRE(held == total);
    }

    const auto pay = payouts(s);
    Chips pot = 0;
    Chips paid = 0;
    for (int i = 0; i < n; ++i) {
      pot += s.total_committed[i];
      paid += pay[static_cast<std::size_t>(i)];
      if (s.folded[i]) {
        REQUIRE(pay[static_cast<std::size_t>(i)] == 0);
      }
    }
    REQUIRE(paid == pot);
  }
}

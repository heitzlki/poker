#include <doctest/doctest.h>

#include <vector>

#include "poker/core/engine.hpp"
#include "test_util.hpp"

using namespace poker;
using namespace poker::core;
using poker::test::hand;

namespace {

// Deck in deal order: two consecutive cards per seat starting left of the
// button, then the board. holes[i] belongs to seat i.
std::vector<Card> deck_for(int num_seats, int button, const std::vector<std::string>& holes,
                           const std::string& board) {
  std::vector<Card> deck;
  for (int k = 0; k < num_seats; ++k) {
    const int seat = (button + 1 + k) % num_seats;
    for (const Card c : hand(holes[static_cast<std::size_t>(seat)])) {
      deck.push_back(c);
    }
  }
  for (const Card c : hand(board)) {
    deck.push_back(c);
  }
  return deck;
}

Action raise_to(Chips amount) {
  return {Action::Kind::kRaise, amount};
}

constexpr Action kFold{Action::Kind::kFold, 0};
constexpr Action kCheck{Action::Kind::kCheck, 0};
constexpr Action kCall{Action::Kind::kCall, 0};

} // namespace

TEST_CASE("heads-up checkdown reaches showdown") {
  const TableConfig cfg{5, 10};
  const std::vector<Chips> stacks{1000, 1000};
  const auto deck = deck_for(2, 0, {"Ah Kh", "2c 7d"}, "Kc Qd Jh 2s 3s");
  State s = new_hand(cfg, stacks, 0, deck);

  CHECK(s.to_act == 0); // heads-up: button posts the small blind and opens
  apply(s, kCall);
  CHECK(s.to_act == 1);
  apply(s, kCheck);

  CHECK(s.street == Street::kFlop);
  CHECK(s.board_count == 3);
  CHECK(s.to_act == 1); // big blind first on every later street
  for (int street = 0; street < 3; ++street) {
    apply(s, kCheck);
    apply(s, kCheck);
  }

  REQUIRE(is_terminal(s));
  const auto pay = payouts(s);
  CHECK(pay[0] == 20); // kings beat deuces
  CHECK(pay[1] == 0);
  CHECK(s.stacks[0] + pay[0] == 1010);
}

TEST_CASE("bet takes the pot when the other seat folds") {
  const TableConfig cfg{5, 10};
  const std::vector<Chips> stacks{1000, 1000};
  const auto deck = deck_for(2, 0, {"Ah Kh", "2c 7d"}, "Kc Qd Jh 2s 3s");
  State s = new_hand(cfg, stacks, 0, deck);

  apply(s, kCall);
  apply(s, kCheck);
  CHECK(s.street == Street::kFlop);

  const auto la = legal_actions(s);
  CHECK(la.can_raise);
  CHECK(la.min_raise_to == 10); // opening bet must be at least the big blind
  apply(s, raise_to(30));
  apply(s, kFold);

  REQUIRE(is_terminal(s));
  CHECK(s.board_count == 3);
  const auto pay = payouts(s);
  CHECK(pay[1] == 50);
  CHECK(pay[0] == 0);
}

TEST_CASE("raise and reraise move the minimum") {
  const TableConfig cfg{5, 10};
  const std::vector<Chips> stacks{1000, 1000};
  const auto deck = deck_for(2, 0, {"Ah Kh", "2c 7d"}, "Kc Qd Jh 2s 3s");
  State s = new_hand(cfg, stacks, 0, deck);

  CHECK(legal_actions(s).min_raise_to == 20);
  apply(s, raise_to(30));
  CHECK(legal_actions(s).min_raise_to == 50);
  apply(s, raise_to(90));
  CHECK(legal_actions(s).min_raise_to == 150);
  apply(s, kCall);

  CHECK(s.street == Street::kFlop);
  CHECK(s.stacks[0] == 910);
  CHECK(s.stacks[1] == 910);
}

TEST_CASE("big blind keeps the option after limps") {
  const TableConfig cfg{5, 10};
  const std::vector<Chips> stacks{1000, 1000, 1000};
  const auto deck = deck_for(3, 0, {"Ah Kh", "2c 7d", "9s 9c"}, "Kc Qd Jh 2s 3s");
  State s = new_hand(cfg, stacks, 0, deck);

  CHECK(s.to_act == 0); // seat left of the big blind opens
  apply(s, kCall);
  CHECK(s.to_act == 1);
  apply(s, kCall);

  CHECK(s.to_act == 2);
  const auto la = legal_actions(s);
  CHECK(la.can_check);
  CHECK(la.can_raise);
  CHECK(la.min_raise_to == 20);

  apply(s, raise_to(30)); // the option reopens the round for the limpers
  CHECK(s.to_act == 0);
  CHECK(legal_actions(s).can_call);
  apply(s, kCall);
  apply(s, kCall);
  CHECK(s.street == Street::kFlop);
}

TEST_CASE("folding to the big blind ends the hand without showdown") {
  const TableConfig cfg{5, 10};
  const std::vector<Chips> stacks{1000, 1000, 1000};
  const auto deck = deck_for(3, 0, {"Ah Kh", "2c 7d", "9s 9c"}, "Kc Qd Jh 2s 3s");
  State s = new_hand(cfg, stacks, 0, deck);

  apply(s, kFold);
  apply(s, kFold);

  REQUIRE(is_terminal(s));
  const auto pay = payouts(s);
  CHECK(pay[2] == 15);
  CHECK(s.stacks[2] + pay[2] == 1005);
}

TEST_CASE("undersized all-in does not reopen the betting") {
  const TableConfig cfg{5, 10};
  const std::vector<Chips> stacks{500, 45, 500};
  const auto deck = deck_for(3, 0, {"Kc Kd", "Ac Ad", "7h 2s"}, "3c 4d 8h 9s Jc");
  State s = new_hand(cfg, stacks, 0, deck);

  apply(s, raise_to(30));

  // The small blind's whole stack is less than a full reraise.
  auto la = legal_actions(s);
  CHECK(la.can_raise);
  CHECK(la.min_raise_to == 45);
  CHECK(la.max_raise_to == 45);
  apply(s, raise_to(45));

  apply(s, kFold); // big blind

  la = legal_actions(s);
  CHECK(la.can_call);
  CHECK(la.call_cost == 15);
  CHECK(!la.can_raise); // seat 0 already acted and no full raise came after
  apply(s, kCall);

  REQUIRE(is_terminal(s));
  const auto pay = payouts(s);
  CHECK(pay[1] == 100); // aces win the lot, including the dead big blind
}

TEST_CASE("three-way all-in builds a side pot") {
  const TableConfig cfg{5, 10};
  const std::vector<Chips> stacks{100, 50, 200};
  const auto deck = deck_for(3, 0, {"Tc 9s", "5c Ac", "Kh Qh"}, "2c 3c 4d 9h Th");
  State s = new_hand(cfg, stacks, 0, deck);

  apply(s, raise_to(100)); // seat 0 all-in
  apply(s, kCall);         // seat 1 all-in for less
  apply(s, kCall);         // seat 2 covers

  REQUIRE(is_terminal(s));
  CHECK(s.board_count == 5);
  const auto pay = payouts(s);
  CHECK(pay[1] == 150); // the wheel takes the main pot
  CHECK(pay[0] == 100); // two pair beats king high in the side pot
  CHECK(pay[2] == 0);
}

TEST_CASE("split pot gives the odd chip to the first seat left of the button") {
  const TableConfig cfg{5, 10};
  const std::vector<Chips> stacks{1000, 1000, 1000};
  const auto deck = deck_for(3, 0, {"2c 3c", "7c 8c", "2d 3d"}, "Ah Kd Qc Jh Ts");
  State s = new_hand(cfg, stacks, 0, deck);

  apply(s, raise_to(21));
  apply(s, kFold); // small blind leaves 5 dead
  apply(s, kCall);

  while (!is_terminal(s)) {
    apply(s, kCheck);
  }

  const auto pay = payouts(s); // both play the board; pot is 47
  CHECK(pay[2] == 24);
  CHECK(pay[0] == 23);
  CHECK(pay[1] == 0);
}

TEST_CASE("blinds can put both players all-in at the deal") {
  const TableConfig cfg{5, 10};
  const std::vector<Chips> stacks{5, 10};
  const auto deck = deck_for(2, 0, {"Ah Kh", "2c 7d"}, "Kc Qd Jh 2s 3s");
  const State s = new_hand(cfg, stacks, 0, deck);

  REQUIRE(is_terminal(s));
  CHECK(s.board_count == 5);
  const auto pay = payouts(s);
  CHECK(pay[0] == 10); // seat 0 can only win what it covers
  CHECK(pay[1] == 5);  // the uncontested rest of the big blind comes back
}

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "poker/core/engine.hpp"
#include "poker/core/shuffle.hpp"
#include "poker/eval/evaluator.hpp"

namespace py = pybind11;
using namespace poker;
using namespace poker::core;

namespace {

// The engine checks its contract with asserts, which are gone in release
// builds, so the binding layer validates everything coming from Python.

Card checked_card(int value) {
  if (value < 0 || value >= eval::kNumCards) {
    throw std::invalid_argument("card must be in [0, 52)");
  }
  return static_cast<Card>(value);
}

std::vector<Card> checked_cards(const std::vector<int>& values) {
  std::vector<Card> cards;
  cards.reserve(values.size());
  eval::CardSet seen = 0;
  for (const int v : values) {
    const Card c = checked_card(v);
    if ((seen & eval::card_bit(c)) != 0) {
      throw std::invalid_argument("duplicate card");
    }
    seen |= eval::card_bit(c);
    cards.push_back(c);
  }
  return cards;
}

int checked_seat(const State& s, int seat) {
  if (seat < 0 || seat >= s.num_seats) {
    throw std::out_of_range("no such seat");
  }
  return seat;
}

Chips pot_size(const State& s) {
  Chips pot = 0;
  for (int i = 0; i < s.num_seats; ++i) {
    pot += s.total_committed[i];
  }
  return pot;
}

void checked_apply(State& s, const Action& a) {
  if (is_terminal(s)) {
    throw std::invalid_argument("the hand is over");
  }
  const LegalActions la = legal_actions(s);
  switch (a.kind) {
  case Action::Kind::kFold:
    break;
  case Action::Kind::kCheck:
    if (!la.can_check) {
      throw std::invalid_argument("cannot check facing a bet");
    }
    break;
  case Action::Kind::kCall:
    if (!la.can_call) {
      throw std::invalid_argument("nothing to call");
    }
    break;
  case Action::Kind::kRaise:
    if (!la.can_raise) {
      throw std::invalid_argument("cannot raise");
    }
    if (a.amount != la.max_raise_to && (a.amount < la.min_raise_to || a.amount > la.max_raise_to)) {
      throw std::invalid_argument("raise total out of range");
    }
    break;
  }
  apply(s, a);
}

} // namespace

PYBIND11_MODULE(pokercore, m) {
  m.doc() = "No-limit hold'em engine";

  py::enum_<Street>(m, "Street")
      .value("PREFLOP", Street::kPreflop)
      .value("FLOP", Street::kFlop)
      .value("TURN", Street::kTurn)
      .value("RIVER", Street::kRiver);

  py::enum_<Action::Kind>(m, "Kind")
      .value("FOLD", Action::Kind::kFold)
      .value("CHECK", Action::Kind::kCheck)
      .value("CALL", Action::Kind::kCall)
      .value("RAISE", Action::Kind::kRaise);

  py::class_<TableConfig>(m, "TableConfig")
      .def(py::init([](Chips small_blind, Chips big_blind) {
             if (small_blind <= 0 || big_blind < small_blind) {
               throw std::invalid_argument("want 0 < small_blind <= big_blind");
             }
             return TableConfig{small_blind, big_blind};
           }),
           py::arg("small_blind") = 5, py::arg("big_blind") = 10)
      .def_readwrite("small_blind", &TableConfig::small_blind)
      .def_readwrite("big_blind", &TableConfig::big_blind);

  py::class_<Action>(m, "Action")
      .def(py::init([](Action::Kind kind, Chips amount) { return Action{kind, amount}; }),
           py::arg("kind"), py::arg("amount") = 0)
      .def_readwrite("kind", &Action::kind)
      .def_readwrite("amount", &Action::amount);

  py::class_<LegalActions>(m, "LegalActions")
      .def_readonly("can_fold", &LegalActions::can_fold)
      .def_readonly("can_check", &LegalActions::can_check)
      .def_readonly("can_call", &LegalActions::can_call)
      .def_readonly("can_raise", &LegalActions::can_raise)
      .def_readonly("call_cost", &LegalActions::call_cost)
      .def_readonly("min_raise_to", &LegalActions::min_raise_to)
      .def_readonly("max_raise_to", &LegalActions::max_raise_to);

  py::class_<State>(m, "State")
      .def_readonly("num_seats", &State::num_seats)
      .def_readonly("button", &State::button)
      .def_readonly("to_act", &State::to_act)
      .def_readonly("street", &State::street)
      .def_readonly("current_bet", &State::current_bet)
      .def_property_readonly("pot", &pot_size)
      .def_property_readonly(
          "stacks",
          [](const State& s) { return std::vector<Chips>(s.stacks, s.stacks + s.num_seats); })
      .def_property_readonly("committed",
                             [](const State& s) {
                               return std::vector<Chips>(s.total_committed,
                                                         s.total_committed + s.num_seats);
                             })
      .def_property_readonly(
          "board",
          [](const State& s) { return std::vector<int>(s.board, s.board + s.board_count); })
      .def_property_readonly(
          "folded",
          [](const State& s) { return std::vector<bool>(s.folded, s.folded + s.num_seats); })
      .def_property_readonly(
          "all_in",
          [](const State& s) { return std::vector<bool>(s.all_in, s.all_in + s.num_seats); })
      .def("hole", [](const State& s, int seat) {
        checked_seat(s, seat);
        return std::make_pair(static_cast<int>(s.hole[seat][0]), static_cast<int>(s.hole[seat][1]));
      });

  m.def(
      "new_hand",
      [](const TableConfig& cfg, const std::vector<Chips>& stacks, int button,
         const std::vector<int>& deck) {
        const int n = static_cast<int>(stacks.size());
        if (n < 2 || n > kMaxSeats) {
          throw std::invalid_argument("want 2 to 6 stacks");
        }
        for (const Chips stack : stacks) {
          if (stack <= 0) {
            throw std::invalid_argument("stacks must be positive");
          }
        }
        if (button < 0 || button >= n) {
          throw std::invalid_argument("button seat out of range");
        }
        const auto cards = checked_cards(deck);
        if (static_cast<int>(cards.size()) < kHoleCards * n + kBoardCards) {
          throw std::invalid_argument("deck too small for the table");
        }
        return new_hand(cfg, stacks, button, cards);
      },
      py::arg("cfg"), py::arg("stacks"), py::arg("button"), py::arg("deck"));

  m.def("legal_actions", &legal_actions, py::arg("state"));
  m.def("apply", &checked_apply, py::arg("state"), py::arg("action"));
  m.def("is_terminal", &is_terminal, py::arg("state"));

  m.def(
      "payouts",
      [](const State& s) {
        if (!is_terminal(s)) {
          throw std::invalid_argument("the hand is not over");
        }
        const auto pay = payouts(s);
        return std::vector<Chips>(pay.begin(), pay.begin() + s.num_seats);
      },
      py::arg("state"));

  m.def(
      "shuffled_deck",
      [](std::uint64_t seed) {
        const auto deck = shuffled_deck(seed);
        return std::vector<int>(deck.begin(), deck.end());
      },
      py::arg("seed"));

  m.def(
      "evaluate",
      [](const std::vector<int>& cards) {
        if (cards.size() < 5 || cards.size() > 7) {
          throw std::invalid_argument("want 5 to 7 cards");
        }
        return eval::evaluate_fast(checked_cards(cards));
      },
      py::arg("cards"));

  m.def(
      "category_name",
      [](eval::HandRank rank) { return std::string(eval::category_name(eval::category(rank))); },
      py::arg("rank"));

  m.def(
      "parse_card",
      [](const std::string& text) -> std::optional<int> {
        const auto card = eval::parse_card(text);
        return card ? std::optional<int>(*card) : std::nullopt;
      },
      py::arg("text"));

  m.def(
      "card_str", [](int card) { return std::string(eval::card_str(checked_card(card)).data()); },
      py::arg("card"));
}

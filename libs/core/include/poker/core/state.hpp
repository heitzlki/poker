#pragma once

#include <cstdint>

#include "poker/eval/cards.hpp"

namespace poker::core {

using eval::Card;
using Chips = std::int64_t;

inline constexpr int kMaxSeats = 6;
inline constexpr int kHoleCards = 2;
inline constexpr int kBoardCards = 5;

enum class Street : std::uint8_t { kPreflop = 0, kFlop, kTurn, kRiver };

struct TableConfig {
  Chips small_blind = 5;
  Chips big_blind = 10;
};

// Raises are to a total: `amount` is the seat's street commitment after the
// action, not the increment. A bet is a raise from zero.
struct Action {
  enum class Kind : std::uint8_t { kFold, kCheck, kCall, kRaise };
  Kind kind = Kind::kFold;
  Chips amount = 0;
};

struct State {
  TableConfig cfg;
  int num_seats = 0;
  int button = 0;
  int to_act = -1; // -1 once the hand is over
  Street street = Street::kPreflop;

  Chips stacks[kMaxSeats] = {};
  Chips street_committed[kMaxSeats] = {};
  Chips total_committed[kMaxSeats] = {};
  bool folded[kMaxSeats] = {};
  bool all_in[kMaxSeats] = {};
  bool acted[kMaxSeats] = {}; // since the last full raise on this street

  Chips current_bet = 0;
  Chips min_raise_delta = 0;

  Card hole[kMaxSeats][kHoleCards] = {};
  Card board[kBoardCards] = {};
  int board_count = 0;

  Card deck[eval::kNumCards] = {};
  int deck_size = 0;
  int next_card = 0;
};

} // namespace poker::core

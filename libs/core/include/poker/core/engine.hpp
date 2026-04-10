#pragma once

#include <array>
#include <span>

#include "poker/core/state.hpp"

namespace poker::core {

struct LegalActions {
  bool can_fold = false;
  bool can_check = false;
  bool can_call = false;
  bool can_raise = false;
  Chips call_cost = 0;    // chips a call adds; the whole stack if that is less
  Chips min_raise_to = 0; // lowest legal raise total, capped at all-in
  Chips max_raise_to = 0; // all-in total
};

// The engine is a pure state machine: no randomness, no I/O. The caller
// supplies a shuffled deck and drives the hand with apply(). Passing an
// action that legal_actions() does not allow is a contract violation
// (checked by assert in debug builds).
State new_hand(const TableConfig& cfg, std::span<const Chips> stacks, int button,
               std::span<const Card> deck);

bool is_terminal(const State& s);
LegalActions legal_actions(const State& s);
void apply(State& s, Action a);

// Pot distribution at the end of the hand, including any uncalled chips
// going back to their owner. Sums to the total committed by all seats.
std::array<Chips, kMaxSeats> payouts(const State& s);

} // namespace poker::core

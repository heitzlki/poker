#include "poker/core/engine.hpp"

#include <algorithm>
#include <cassert>
#include <type_traits>

#include "poker/eval/evaluator.hpp"

namespace poker::core {

static_assert(std::is_trivially_copyable_v<State>);

namespace {

int next_seat(const State& s, int seat) {
  return (seat + 1) % s.num_seats;
}

bool can_act(const State& s, int seat) {
  return !s.folded[seat] && !s.all_in[seat];
}

int actable_count(const State& s) {
  int n = 0;
  for (int i = 0; i < s.num_seats; ++i) {
    n += can_act(s, i) ? 1 : 0;
  }
  return n;
}

int alive_count(const State& s) {
  int n = 0;
  for (int i = 0; i < s.num_seats; ++i) {
    n += s.folded[i] ? 0 : 1;
  }
  return n;
}

// A seat still owes a decision if it is behind the current bet, or has not
// acted since the last full raise and there is someone left to bet against.
bool needs_action(const State& s, int seat, int actable) {
  if (!can_act(s, seat)) {
    return false;
  }
  if (s.street_committed[seat] < s.current_bet) {
    return true;
  }
  return !s.acted[seat] && actable >= 2;
}

int find_to_act(const State& s, int from) {
  const int actable = actable_count(s);
  int seat = from;
  for (int i = 0; i < s.num_seats; ++i, seat = next_seat(s, seat)) {
    if (needs_action(s, seat, actable)) {
      return seat;
    }
  }
  return -1;
}

Card draw(State& s) {
  assert(s.next_card < s.deck_size);
  return s.deck[s.next_card++];
}

void commit(State& s, int seat, Chips amount) {
  assert(amount >= 0 && amount <= s.stacks[seat]);
  s.stacks[seat] -= amount;
  s.street_committed[seat] += amount;
  s.total_committed[seat] += amount;
  if (s.stacks[seat] == 0) {
    s.all_in[seat] = true;
  }
}

void begin_street(State& s);

void close_street(State& s) {
  if (s.street == Street::kRiver) {
    s.to_act = -1;
    return;
  }
  s.street = static_cast<Street>(static_cast<int>(s.street) + 1);
  begin_street(s);
}

void begin_street(State& s) {
  for (int i = 0; i < s.num_seats; ++i) {
    s.street_committed[i] = 0;
    s.acted[i] = false;
  }
  s.current_bet = 0;
  s.min_raise_delta = s.cfg.big_blind;

  const int cards = s.street == Street::kFlop ? 3 : 1;
  for (int i = 0; i < cards; ++i) {
    s.board[s.board_count++] = draw(s);
  }

  s.to_act = find_to_act(s, next_seat(s, s.button));
  if (s.to_act < 0) {
    close_street(s);
  }
}

} // namespace

State new_hand(const TableConfig& cfg, std::span<const Chips> stacks, int button,
               std::span<const Card> deck) {
  const int n = static_cast<int>(stacks.size());
  assert(n >= 2 && n <= kMaxSeats);
  assert(button >= 0 && button < n);
  assert(static_cast<int>(deck.size()) >= kHoleCards * n + kBoardCards);
  assert(deck.size() <= static_cast<std::size_t>(eval::kNumCards));

  State s;
  s.cfg = cfg;
  s.num_seats = n;
  s.button = button;
  s.street = Street::kPreflop;
  for (int i = 0; i < n; ++i) {
    assert(stacks[static_cast<std::size_t>(i)] > 0);
    s.stacks[i] = stacks[static_cast<std::size_t>(i)];
  }
  std::copy(deck.begin(), deck.end(), s.deck);
  s.deck_size = static_cast<int>(deck.size());

#ifndef NDEBUG
  eval::CardSet seen = 0;
  for (const Card c : deck) {
    assert((seen & eval::card_bit(c)) == 0);
    seen |= eval::card_bit(c);
  }
#endif

  // Heads-up the button posts the small blind and acts first preflop.
  const int sb = n == 2 ? button : next_seat(s, button);
  const int bb = next_seat(s, sb);
  commit(s, sb, std::min(cfg.small_blind, s.stacks[sb]));
  commit(s, bb, std::min(cfg.big_blind, s.stacks[bb]));
  s.current_bet = cfg.big_blind;
  s.min_raise_delta = cfg.big_blind;

  // Two consecutive cards per seat, starting left of the button. No burns.
  int seat = next_seat(s, button);
  for (int i = 0; i < n; ++i, seat = next_seat(s, seat)) {
    s.hole[seat][0] = draw(s);
    s.hole[seat][1] = draw(s);
  }

  s.to_act = find_to_act(s, n == 2 ? button : next_seat(s, bb));
  if (s.to_act < 0) {
    close_street(s); // blinds put everyone all-in; run out the board
  }
  return s;
}

bool is_terminal(const State& s) {
  return s.to_act < 0;
}

LegalActions legal_actions(const State& s) {
  LegalActions la;
  if (is_terminal(s)) {
    return la;
  }
  const int seat = s.to_act;
  const Chips committed = s.street_committed[seat];
  const Chips owed = s.current_bet - committed;

  la.can_fold = true;
  if (owed == 0) {
    la.can_check = true;
  } else {
    la.can_call = true;
    la.call_cost = std::min(owed, s.stacks[seat]);
  }

  // A seat that already acted may not raise again unless a full raise
  // reopened the betting; an undersized all-in behind it does not.
  const Chips all_in_total = committed + s.stacks[seat];
  if (all_in_total > s.current_bet && !s.acted[seat]) {
    la.can_raise = true;
    la.max_raise_to = all_in_total;
    la.min_raise_to = std::min(s.current_bet + s.min_raise_delta, all_in_total);
  }
  return la;
}

void apply(State& s, Action a) {
  assert(!is_terminal(s));
  const int seat = s.to_act;
  const LegalActions la = legal_actions(s);
  static_cast<void>(la);

  switch (a.kind) {
  case Action::Kind::kFold:
    assert(la.can_fold);
    s.folded[seat] = true;
    if (alive_count(s) == 1) {
      s.to_act = -1;
      return;
    }
    break;
  case Action::Kind::kCheck:
    assert(la.can_check);
    break;
  case Action::Kind::kCall:
    assert(la.can_call);
    commit(s, seat, std::min(s.current_bet - s.street_committed[seat], s.stacks[seat]));
    break;
  case Action::Kind::kRaise: {
    assert(la.can_raise);
    const Chips to = a.amount;
    assert(to == la.max_raise_to || (to >= la.min_raise_to && to <= la.max_raise_to));
    const Chips delta = to - s.current_bet;
    commit(s, seat, to - s.street_committed[seat]);
    s.current_bet = to;
    if (delta >= s.min_raise_delta) {
      s.min_raise_delta = delta;
      for (int i = 0; i < s.num_seats; ++i) {
        if (i != seat) {
          s.acted[i] = false;
        }
      }
    }
    break;
  }
  }
  s.acted[seat] = true;

  s.to_act = find_to_act(s, next_seat(s, seat));
  if (s.to_act < 0) {
    close_street(s);
  }
}

std::array<Chips, kMaxSeats> payouts(const State& s) {
  assert(is_terminal(s));
  std::array<Chips, kMaxSeats> out{};

  Chips pot_total = 0;
  int last_alive = -1;
  for (int i = 0; i < s.num_seats; ++i) {
    pot_total += s.total_committed[i];
    if (!s.folded[i]) {
      last_alive = i;
    }
  }
  if (alive_count(s) == 1) {
    out[static_cast<std::size_t>(last_alive)] = pot_total;
    return out;
  }

  assert(s.board_count == kBoardCards);
  eval::HandRank rank[kMaxSeats] = {};
  for (int i = 0; i < s.num_seats; ++i) {
    if (s.folded[i]) {
      continue;
    }
    Card seven[kHoleCards + kBoardCards];
    seven[0] = s.hole[i][0];
    seven[1] = s.hole[i][1];
    std::copy(s.board, s.board + kBoardCards, seven + kHoleCards);
    rank[i] = eval::evaluate_fast(seven);
  }

  // One pot slice per distinct commitment level of a live seat; folded
  // chips fall into whichever slices they reach.
  Chips levels[kMaxSeats];
  int n_levels = 0;
  for (int i = 0; i < s.num_seats; ++i) {
    if (!s.folded[i]) {
      levels[n_levels++] = s.total_committed[i];
    }
  }
  std::sort(levels, levels + n_levels);
  n_levels = static_cast<int>(std::unique(levels, levels + n_levels) - levels);

  Chips prev = 0;
  for (int l = 0; l < n_levels; ++l) {
    const Chips level = levels[l];
    // The top slice also absorbs folded chips beyond the highest live
    // commitment (a blind poster can fold ahead of shorter all-ins).
    const bool top = l == n_levels - 1;
    Chips pot = 0;
    for (int i = 0; i < s.num_seats; ++i) {
      const Chips in = top ? s.total_committed[i] : std::min(s.total_committed[i], level);
      pot += in - std::min(s.total_committed[i], prev);
    }

    eval::HandRank best = 0;
    for (int i = 0; i < s.num_seats; ++i) {
      if (!s.folded[i] && s.total_committed[i] >= level) {
        best = std::max(best, rank[i]);
      }
    }
    int n_winners = 0;
    bool winner[kMaxSeats] = {};
    for (int i = 0; i < s.num_seats; ++i) {
      if (!s.folded[i] && s.total_committed[i] >= level && rank[i] == best) {
        winner[i] = true;
        ++n_winners;
      }
    }

    // Odd chips go to the first winners left of the button.
    const Chips share = pot / n_winners;
    Chips odd = pot % n_winners;
    int seat = next_seat(s, s.button);
    for (int i = 0; i < s.num_seats; ++i, seat = next_seat(s, seat)) {
      if (winner[seat]) {
        out[static_cast<std::size_t>(seat)] += share + (odd > 0 ? 1 : 0);
        odd = std::max<Chips>(odd - 1, 0);
      }
    }
    prev = level;
  }

#ifndef NDEBUG
  Chips distributed = 0;
  for (const Chips c : out) {
    distributed += c;
  }
  assert(distributed == pot_total);
#endif
  return out;
}

} // namespace poker::core

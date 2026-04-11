"""Plays one hand of no-limit hold'em through the pokercore module and
narrates it.

Build the module with `just py`, then run this with a Python matching the
version the module was built against (the .so is tagged, e.g.
pokercore.cpython-312-...). vcpkg ships one alongside the build:

    PYTHONPATH=build/release-py/bindings/python \
      build/release-py/vcpkg_installed/arm64-osx/tools/python3/python3.12 \
      bindings/python/example.py
"""

import pokercore as pc


def fmt(cards):
    """Cards cross the boundary as ints 0..51; card_str makes them readable."""
    return " ".join(pc.card_str(c) for c in cards)


def choose(state, legal):
    """A toy policy: min-raise any pocket pair, otherwise take the cheap
    continue. Real logic would look at state.board, state.pot and so on."""
    hole = state.hole(state.to_act)
    pair = hole[0] // 4 == hole[1] // 4  # rank is card // 4
    if pair and legal.can_raise:
        return pc.Action(pc.Kind.RAISE, legal.min_raise_to)
    if legal.can_call:
        return pc.Action(pc.Kind.CALL)
    return pc.Action(pc.Kind.CHECK)


def main():
    cfg = pc.TableConfig(small_blind=5, big_blind=10)
    stacks = [1000, 1000, 1000]
    deck = pc.shuffled_deck(seed=42)

    # The engine is a state machine: it holds no randomness of its own, the
    # shuffled deck goes in and every change happens through apply().
    state = pc.new_hand(cfg, stacks, 0, deck)

    for seat in range(state.num_seats):
        print(f"seat {seat} holds {fmt(state.hole(seat))}")

    seen_board = 0
    while not pc.is_terminal(state):
        if len(state.board) > seen_board:
            print(f"board: {fmt(state.board)}   pot {state.pot}")
            seen_board = len(state.board)

        legal = pc.legal_actions(state)
        action = choose(state, legal)
        amount = f" to {action.amount}" if action.kind == pc.Kind.RAISE else ""
        print(f"seat {state.to_act} {action.kind.name.lower()}{amount}")
        pc.apply(state, action)

    print(f"final board: {fmt(state.board)}")
    for seat in range(state.num_seats):
        if not state.folded[seat]:
            rank = pc.evaluate(list(state.hole(seat)) + list(state.board))
            print(f"seat {seat} shows {fmt(state.hole(seat))}: "
                  f"{pc.category_name(rank)}")

    pay = pc.payouts(state)
    for seat, won in enumerate(pay):
        if won:
            print(f"seat {seat} wins {won}")

    # Chips never appear or vanish; this holds after every apply() too.
    assert sum(state.stacks) + sum(pay) == sum(stacks)


if __name__ == "__main__":
    main()

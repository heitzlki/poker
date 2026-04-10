"""End-to-end check of the pokercore module: cards round-trip, illegal
actions raise, and random-ish playouts conserve chips."""

import pokercore as pc


def play_hand(seed, seats, button):
    deck = pc.shuffled_deck(seed)
    stacks = [37 + (seed * 13 + i * 71) % 400 for i in range(seats)]
    state = pc.new_hand(pc.TableConfig(5, 10), stacks, button, deck)
    bankroll = sum(stacks)

    steps = 0
    while not pc.is_terminal(state):
        steps += 1
        assert steps < 500, "hand does not terminate"
        la = pc.legal_actions(state)
        roll = (seed + steps * 7919 + state.to_act) % 10
        if la.can_raise and roll >= 8:
            action = pc.Action(pc.Kind.RAISE, la.min_raise_to)
        elif la.can_call:
            action = pc.Action(pc.Kind.CALL) if roll else pc.Action(pc.Kind.FOLD)
        else:
            action = pc.Action(pc.Kind.CHECK)
        pc.apply(state, action)
        assert sum(state.stacks) + state.pot == bankroll

    pay = pc.payouts(state)
    assert sum(pay) == state.pot
    for seat in range(seats):
        assert not (state.folded[seat] and pay[seat] > 0)


def main():
    assert pc.card_str(pc.parse_card("As")) == "As"
    assert pc.parse_card("Xx") is None

    royal = [pc.parse_card(c) for c in ("Ah", "Kh", "Qh", "Jh", "Th")]
    assert pc.category_name(pc.evaluate(royal)) == "straight flush"

    state = pc.new_hand(pc.TableConfig(5, 10), [1000, 1000], 0, pc.shuffled_deck(1))
    try:
        pc.apply(state, pc.Action(pc.Kind.CHECK))
    except ValueError:
        pass
    else:
        raise AssertionError("checking into a bet must raise")

    hands = 0
    for seed in range(200):
        for seats in (2, 4, 6):
            play_hand(seed, seats, seed % seats)
            hands += 1
    print(f"ok, {hands} hands")


main()

# Rules and encoding decisions

The engine's behavior is defined here, not by the code: when the two disagree, the
code is wrong. Anything that talks to the engine (CLI, Python, tests) relies on
these definitions.

## Card encoding

- A card is an integer in `[0, 52)`: `card = rank * 4 + suit`.
- Ranks: `0` = deuce through `12` = ace.
- Suits: `0` = clubs, `1` = diamonds, `2` = hearts, `3` = spades.
- Text form is rank then suit: ranks `23456789TJQKA`, suits `cdhs` (e.g. `As`, `Td`).
- A set of cards is a `uint64` bitmask; bit `i` set means card `i` is present.

## Hand rank encoding (high hands)

`HandRank` is a `uint32`. **Greater value wins.** Equal value is an exact tie.

Bit layout:

```
[23:20] category   [19:16] t0   [15:12] t1   [11:8] t2   [7:4] t3   [3:0] t4
```

Categories, ascending: high card `0`, one pair `1`, two pair `2`, three of a kind `3`,
straight `4`, flush `5`, full house `6`, four of a kind `7`, straight flush `8`.

`t0..t4` are category-specific tiebreak ranks (card ranks, `0`–`12`), most significant
first; slots a category does not use are `0`:

| Category        | t0        | t1        | t2     | t3     | t4     |
| --------------- | --------- | --------- | ------ | ------ | ------ |
| High card       | kicker 1  | kicker 2  | k3     | k4     | k5     |
| One pair        | pair      | kicker 1  | k2     | k3     | —      |
| Two pair        | high pair | low pair  | kicker | —      | —      |
| Trips           | trips     | kicker 1  | k2     | —      | —      |
| Straight        | high card | —         | —      | —      | —      |
| Flush           | kicker 1  | kicker 2  | k3     | k4     | k5     |
| Full house      | trips     | pair      | —      | —      | —      |
| Quads           | quads     | kicker    | —      | —      | —      |
| Straight flush  | high card | —         | —      | —      | —      |

- The wheel (A-5-4-3-2) is a five-high straight: `t0 = 3` (the five). The ace never
  plays high and low in the same straight.
- Suits never break ties.

## No-limit hold'em

### Table and dealing

- 2 to 6 seats. The button seat is an input to each hand.
- Blinds: small blind left of the button, big blind next. Heads-up the button posts
  the small blind and acts first preflop; on every later street the big blind acts
  first. A blind short of a full stack posts all-in.
- Dealing order from the caller's deck: two consecutive cards per seat starting left
  of the button, then flop, turn, river. No burn cards.

### Betting

- Raises are declared as a total ("raise to"), never as an increment. A bet is a
  raise from zero.
- An opening bet must be at least the big blind. A raise must add at least the size
  of the last full raise on that street (the big blind before any raise). Both
  minimums bend for a player going all-in below them.
- An all-in raise below the minimum does not reopen the betting: players who already
  acted may call the extra amount or fold, but not raise again.
- The nominal big blind sets the preflop bet even when the poster is all-in for
  less; anything nobody can contest comes back in the payout.
- Folding is legal whenever it is a player's turn, including facing no bet.
- A street ends when every player still able to act has matched the current bet and
  acted since the last full raise. When fewer than two players can act, the
  remaining board runs out and the hand goes to showdown.

### Showdown and payouts

- One pot per distinct all-in level among the players still in the hand, each
  contested only by those who covered it. Folded chips fall into whichever pots
  they reach; anything folded beyond the highest live commitment is dead money and
  joins the last pot.
- An uncalled bet goes back to the bettor (it forms a pot only they are eligible
  for).
- Tied hands split their pot evenly; leftover odd chips go one each to the winners
  closest to the left of the button.

## Randomness

- The engine itself never draws randomness; it consumes a deck the caller shuffled.
- Shuffling (`poker/core/shuffle.hpp`) is an explicit Fisher–Yates with Lemire's
  unbiased bounded-integer method over SplitMix64. `std::shuffle` and
  `std::uniform_int_distribution` are implementation-defined and would make the same
  seed deal different hands on different standard libraries, so they are not used
  anywhere.

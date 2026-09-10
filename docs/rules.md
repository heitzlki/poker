# Rules and encoding decisions

This file is the contract between every implementation of the engine (C++, later Rust),
every bot author, and every adapter. A behavior is not defined by any implementation;
it is defined here and enforced by the conformance corpus.

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

## Randomness

- Shuffling is an explicit Fisher–Yates with Lemire's unbiased bounded-integer method
  over a specified generator. `std::shuffle` and language-native shuffles are banned in
  engine code: their output is implementation-defined and would break cross-language
  determinism.
- The test suite's reproducible generator is SplitMix64.

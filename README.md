# poker

A no-limit hold'em engine in C++20, with a terminal client and Python bindings.

The engine is a pure state machine: you hand it stacks, a button seat and a
shuffled deck, then drive the hand with `legal_actions` / `apply` until it is
over. It does no I/O, draws no randomness, and a `State` is a flat struct you
can copy around freely. Up to six seats, with all the awkward parts done
properly: min-raise rules, undersized all-ins, side pots, split pots.
The rules and encodings are written down in [docs/rules.md](docs/rules.md);
the code follows the document, not the other way around.

## Playing

```sh
just play                          # you against five bots
poker play --bots 2 --stakes 25/50 --seed 42
```

At the table: `fold` (f), `check` (x), `call` (c), `raise 60` (r, bet, b),
`allin` (a), `help`, `quit`. Raises are always to a total, so `raise 60` means
"make my bet 60", and a bare enter checks or calls. On a terminal the game
draws a fixed table panel with a running log underneath (suits in color,
your name and the bots' told apart by color too):

```
  POKER · NLHE 5/10 · 3 seats
──────────────────────────────────────────────────────────
   1 you      2918  in 24    A♠ 8♣   ◀ to act
   2 bot1        5  in 24    ?? ??
   3 bot2        5  in 24    ?? ??
                   pot 72    T♠ Q♦ 9♠ · ·
──────────────────────────────────────────────────────────
 00:41:45  [you]   show  4♠ 9♥ — flush
 00:41:45  [bot1]  shows 6♣ 5♥ — flush
 00:41:45  [bot2]  shows T♦ 8♠ — high card
 00:41:45  [you]   win  2913
 00:41:45  press enter for the next hand

 00:41:45  — hand #2, button bot1 —
 00:41:45  [bot2]  posts 5
 00:41:45  [you]   post  10
 00:41:45  [bot1]  raises to 24
 00:41:45  [bot2]  calls 19
 00:41:45  [you]   call  14

 00:41:45  flop    T♠ Q♦ 9♠
 00:41:45  [bot2]  checks
 [f]old  [x]check  [r]aise 10..2918  [a]ll-in  [?]help  [q]uit  enter checks
 >
```

When output is piped the game falls back to a plain line-by-line transcript,
which is also what the test suite drives.

Two more subcommands: `poker eval "AhKd 7c8c9h"` ranks a hand, and
`poker sim --hands 100000` plays bot-vs-bot hands as a soak test and
throughput check.

## Using the engine

```cpp
#include <poker/core/engine.hpp>
#include <poker/core/shuffle.hpp>

using namespace poker::core;

const auto deck = shuffled_deck(seed);
State s = new_hand({.small_blind = 5, .big_blind = 10}, stacks, button, deck);
while (!is_terminal(s)) {
  const LegalActions la = legal_actions(s);
  apply(s, decide(la)); // your policy
}
const auto winnings = payouts(s);
```

The same loop from Python (`just py` builds and tests the module):

```python
import pokercore as pc

s = pc.new_hand(pc.TableConfig(5, 10), [1000, 1000], 0, pc.shuffled_deck(7))
while not pc.is_terminal(s):
    la = pc.legal_actions(s)
    pc.apply(s, pc.Action(pc.Kind.CALL if la.can_call else pc.Kind.CHECK))
print(pc.payouts(s))
```

## Building

Needs CMake 3.25+, Ninja, a C++20 compiler and
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. The engine
itself has no dependencies; tests, benchmarks and bindings pull theirs through
vcpkg features.

```sh
just test          # build and run everything under ASan/UBSan
just bench         # hand evaluator benchmark, release mode
just play          # build the CLI and sit down
just py            # build the Python module and run its tests
just fmt
```

Without just: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`.
Presets: `debug`, `release`, `asan`, `release-py`.

## Correctness

The hand evaluator does about 34M 7-card evaluations per second on an Apple
M-series core. A second, deliberately slow evaluator exists only to check it:
the tests compare the two on every one of the 2,598,960 possible 5-card hands,
verify the category counts against the known odds, and fuzz random 7-card
hands on top. The betting engine gets the same treatment — thousands of
random full hands driven only through `legal_actions`, with chip conservation
asserted after every action.

Apache-2.0.

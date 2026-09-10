#include <nanobench.h>

#include <array>
#include <cstdint>
#include <vector>

#include "poker/eval/evaluator.hpp"

using namespace poker::eval;

namespace {

// SplitMix64, matching the test suite's generator.
std::uint64_t next(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

std::vector<std::array<Card, 7>> random_hands(std::size_t n, std::uint64_t seed) {
  std::vector<std::array<Card, 7>> hands(n);
  std::array<Card, kNumCards> deck{};
  for (int i = 0; i < kNumCards; ++i) {
    deck[static_cast<std::size_t>(i)] = static_cast<Card>(i);
  }
  for (auto& hand : hands) {
    for (std::uint32_t i = 0; i < 7; ++i) {
      const auto r = static_cast<std::uint32_t>(next(seed) >> 32);
      const std::uint32_t j =
          i +
          static_cast<std::uint32_t>(
              (static_cast<std::uint64_t>(r) * (static_cast<std::uint32_t>(kNumCards) - i)) >> 32);
      std::swap(deck[i], deck[j]);
    }
    std::copy_n(deck.begin(), 7, hand.begin());
  }
  return hands;
}

void bench(ankerl::nanobench::Bench& b, const char* name, EvaluateFn fn,
           const std::vector<std::array<Card, 7>>& hands) {
  b.batch(hands.size()).run(name, [&] {
    std::uint64_t acc = 0;
    for (const auto& hand : hands) {
      acc += fn(hand);
    }
    ankerl::nanobench::doNotOptimizeAway(acc);
  });
}

} // namespace

int main() {
  const auto hands = random_hands(10000, 20260909);

  ankerl::nanobench::Bench b;
  b.title("7-card hand evaluation").unit("eval").minEpochIterations(50);
  bench(b, "evaluate_ref", evaluate_ref, hands);
  bench(b, "evaluate_fast", evaluate_fast, hands);
  return 0;
}

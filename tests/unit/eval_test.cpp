#include <doctest/doctest.h>

#include "poker/eval/evaluator.hpp"
#include "test_util.hpp"

using namespace poker;
using namespace poker::eval;
using poker::test::hand;

namespace {

struct Impl {
  const char* name;
  EvaluateFn fn;
};

constexpr Impl kImpls[] = {{"ref", evaluate_ref}, {"fast", evaluate_fast}};

HandCategory cat_of(EvaluateFn fn, const std::vector<Card>& cards) {
  return category(fn(cards));
}

} // namespace

TEST_CASE("five-card categories") {
  for (const auto& impl : kImpls) {
    CAPTURE(impl.name);
    CHECK(cat_of(impl.fn, hand("As Ks Qs Js Ts")) == HandCategory::kStraightFlush);
    CHECK(cat_of(impl.fn, hand("9h 8h 7h 6h 5h")) == HandCategory::kStraightFlush);
    CHECK(cat_of(impl.fn, hand("Ac Ad Ah As 2c")) == HandCategory::kQuads);
    CHECK(cat_of(impl.fn, hand("Kc Kd Kh 2s 2c")) == HandCategory::kFullHouse);
    CHECK(cat_of(impl.fn, hand("Ad Kd 9d 6d 3d")) == HandCategory::kFlush);
    CHECK(cat_of(impl.fn, hand("Tc 9d 8h 7s 6c")) == HandCategory::kStraight);
    CHECK(cat_of(impl.fn, hand("7c 7d 7h Ks 2c")) == HandCategory::kTrips);
    CHECK(cat_of(impl.fn, hand("Jc Jd 4h 4s Ac")) == HandCategory::kTwoPair);
    CHECK(cat_of(impl.fn, hand("Tc Td Ah 7s 3c")) == HandCategory::kOnePair);
    CHECK(cat_of(impl.fn, hand("Ac Jd 9h 6s 3c")) == HandCategory::kHighCard);
  }
}

TEST_CASE("the wheel is a five-high straight") {
  for (const auto& impl : kImpls) {
    CAPTURE(impl.name);
    const auto wheel = impl.fn(hand("Ah 2c 3d 4s 5c"));
    CHECK(category(wheel) == HandCategory::kStraight);
    // Five-high: loses to a six-high straight, ace does not play high.
    CHECK(wheel < impl.fn(hand("2h 3c 4d 5s 6c")));
  }
}

TEST_CASE("the steel wheel is the lowest straight flush") {
  for (const auto& impl : kImpls) {
    CAPTURE(impl.name);
    const auto steel = impl.fn(hand("Ah 2h 3h 4h 5h"));
    CHECK(category(steel) == HandCategory::kStraightFlush);
    CHECK(steel < impl.fn(hand("2s 3s 4s 5s 6s")));
    CHECK(steel > impl.fn(hand("Ac Ad Ah As Kc"))); // still beats quad aces
  }
}

TEST_CASE("kickers order hands within a category") {
  for (const auto& impl : kImpls) {
    CAPTURE(impl.name);
    CHECK(impl.fn(hand("Ac Kd Qh 7s 2c")) > impl.fn(hand("Ac Kd Qh 6s 4c")));
    CHECK(impl.fn(hand("Tc Td Ah 7s 3c")) > impl.fn(hand("Tc Td Ah 6s 5c")));
    CHECK(impl.fn(hand("Jc Jd 4h 4s Ac")) > impl.fn(hand("Jc Jd 4h 4s Kc")));
    CHECK(impl.fn(hand("Kc Kd Kh 2s 2c")) > impl.fn(hand("Qc Qd Qh As Ac")));
    CHECK(impl.fn(hand("Ac Ad Ah As 2c")) > impl.fn(hand("Kc Kd Kh Ks Ac")));
    // Identical hands in different suits tie exactly.
    CHECK(impl.fn(hand("Ac Kd Qh 7s 2c")) == impl.fn(hand("Ad Kh Qs 7c 2d")));
  }
}

TEST_CASE("category ordering is total") {
  for (const auto& impl : kImpls) {
    CAPTURE(impl.name);
    const HandRank ladder[] = {
        impl.fn(hand("Ac Jd 9h 6s 3c")), // high card
        impl.fn(hand("2c 2d 3h 4s 5d")), // one pair (weakest-ish)
        impl.fn(hand("Jc Jd 4h 4s Ac")), // two pair
        impl.fn(hand("7c 7d 7h Ks 2c")), // trips
        impl.fn(hand("Ah 2c 3d 4s 5c")), // straight (wheel, weakest straight)
        impl.fn(hand("2d 3d 4d 5d 7d")), // flush (weakest flush)
        impl.fn(hand("2c 2d 2h 3s 3c")), // full house (weakest)
        impl.fn(hand("2c 2d 2h 2s 3c")), // quads (weakest)
        impl.fn(hand("Ah 2h 3h 4h 5h")), // straight flush (weakest)
    };
    for (std::size_t i = 1; i < std::size(ladder); ++i) {
      CHECK(ladder[i - 1] < ladder[i]);
    }
  }
}

TEST_CASE("seven cards: best five play") {
  for (const auto& impl : kImpls) {
    CAPTURE(impl.name);
    // Board pairs the ace, but the flush is better than two pair.
    CHECK(cat_of(impl.fn, hand("Ah Kh 2c Ad 5h 9h 3h")) == HandCategory::kFlush);
    // Three pairs: best two play with the best remaining kicker (the ace).
    const auto three_pairs = impl.fn(hand("Kc Kd Qh Qs 2c 2d Ah"));
    CHECK(category(three_pairs) == HandCategory::kTwoPair);
    CHECK(three_pairs == impl.fn(hand("Kc Kd Qh Qs Ah")));
    // Two trips make a full house of the higher trips over the lower pair.
    CHECK(impl.fn(hand("8c 8d 8h 5s 5c 5d Ah")) == impl.fn(hand("8c 8d 8h 5s 5c")));
    // Quads plus trips: quads with the trips rank as kicker.
    CHECK(impl.fn(hand("9c 9d 9h 9s Kc Kd Kh")) == impl.fn(hand("9c 9d 9h 9s Kc")));
    // Straight on the board, one hole card completes a higher straight.
    CHECK(impl.fn(hand("5c 6d 7h 8s 9c Tc 2d")) == impl.fn(hand("6d 7h 8s 9c Tc")));
    // Six cards work too.
    CHECK(cat_of(impl.fn, hand("Ac Ad Ah As 2c 2d")) == HandCategory::kQuads);
  }
}

TEST_CASE("flush uses the best five of six suited cards") {
  for (const auto& impl : kImpls) {
    CAPTURE(impl.name);
    CHECK(impl.fn(hand("Ah Kh Qh Jh 9h 2h 2c")) == impl.fn(hand("Ah Kh Qh Jh 9h")));
  }
}

TEST_CASE("card parsing round-trips") {
  for (int c = 0; c < kNumCards; ++c) {
    const auto card = static_cast<Card>(c);
    const auto s = card_str(card);
    const auto parsed = parse_card(std::string_view{s.data(), 2});
    REQUIRE(parsed.has_value());
    CHECK(*parsed == card);
  }
  CHECK(!parse_card("Xx").has_value());
  CHECK(!parse_card("A").has_value());
  CHECK(!parse_card("Asd").has_value());
}

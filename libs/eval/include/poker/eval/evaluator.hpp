#pragma once

#include <span>

#include "poker/eval/cards.hpp"
#include "poker/eval/hand_rank.hpp"

namespace poker::eval {

// Both evaluators take 5 to 7 distinct cards and return the rank of the best
// 5-card high hand. evaluate_ref is the readable reference implementation and
// permanent differential-testing oracle; evaluate_fast is the production path.
HandRank evaluate_ref(std::span<const Card> cards);
HandRank evaluate_fast(std::span<const Card> cards);

using EvaluateFn = HandRank (*)(std::span<const Card>);

} // namespace poker::eval

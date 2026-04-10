#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <termios.h>
#include <unistd.h>

#include "poker/core/engine.hpp"
#include "poker/core/shuffle.hpp"
#include "poker/eval/evaluator.hpp"

using namespace poker;
using namespace poker::core;

namespace {

void print_usage() {
  std::fputs("usage: poker <command> [options]\n"
             "\n"
             "commands:\n"
             "  play    Play offline against bots, in the terminal\n"
             "          --bots <n>       opponents, 1-5 (default 5)\n"
             "          --stakes <sb/bb> blinds (default 5/10)\n"
             "          --seed <u64>     deterministic shuffles\n"
             "  eval    Rank a poker hand (5-7 cards), e.g. \"AhKd 7c8c9h\"\n"
             "          --json           machine-readable output\n"
             "  sim     Headless bot-vs-bot simulation, for benchmarks and soak tests\n"
             "          --hands <n>      hands to play (default 10000)\n"
             "          --seed <u64>     rng seed (default 0)\n"
             "          --bots <n>       seats, 2-6 (default 5)\n"
             "          --json           machine-readable output\n",
             stderr);
}

std::optional<std::uint64_t> parse_u64(std::string_view s) {
  std::uint64_t value = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || ptr != s.data() + s.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<Chips> parse_chips(std::string_view s) {
  const auto v = parse_u64(s);
  if (!v || *v > (std::uint64_t{1} << 62)) {
    return std::nullopt;
  }
  return static_cast<Chips>(*v);
}

// "5/10" -> {5, 10}
std::optional<TableConfig> parse_stakes(std::string_view s) {
  const auto slash = s.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }
  const auto sb = parse_chips(s.substr(0, slash));
  const auto bb = parse_chips(s.substr(slash + 1));
  if (!sb || !bb || *sb <= 0 || *bb < *sb) {
    return std::nullopt;
  }
  return TableConfig{*sb, *bb};
}

// Cards in any spacing: "AhKd 7c8c9h", "Ah Kd ...".
std::optional<std::vector<Card>> parse_cards(std::string_view text) {
  std::vector<Card> cards;
  eval::CardSet seen = 0;
  std::string token;
  for (const char ch : text) {
    if (ch == ' ' || ch == ',' || ch == '\t') {
      continue;
    }
    token.push_back(ch);
    if (token.size() < 2) {
      continue;
    }
    const auto card = eval::parse_card(token);
    if (!card || (seen & eval::card_bit(*card)) != 0) {
      return std::nullopt;
    }
    seen |= eval::card_bit(*card);
    cards.push_back(*card);
    token.clear();
  }
  if (!token.empty()) {
    return std::nullopt;
  }
  return cards;
}

// Suit symbols and red diamonds/hearts on a terminal; plain "9h" when piped
// or NO_COLOR is set, so scripts and tests see stable ASCII.
bool use_color() {
  static const bool enabled = [] {
    if (std::getenv("CLICOLOR_FORCE") != nullptr) {
      return true;
    }
    return isatty(STDOUT_FILENO) == 1 && std::getenv("NO_COLOR") == nullptr;
  }();
  return enabled;
}

std::string card_display(Card c) {
  if (!use_color()) {
    return eval::card_str(c).data();
  }
  static const char* kSymbols[] = {"♣", "♦", "♥", "♠"}; // c d h s
  const int suit = eval::suit_of(c);
  const bool red = suit == 1 || suit == 2;
  std::string out;
  if (red) {
    out += "\033[31m";
  }
  out += eval::kRankChars[static_cast<std::size_t>(eval::rank_of(c))];
  out += kSymbols[suit];
  if (red) {
    out += "\033[0m";
  }
  return out;
}

std::string cards_str(const Card* cards, int n) {
  std::string out;
  for (int i = 0; i < n; ++i) {
    if (i > 0) {
      out += ' ';
    }
    out += card_display(cards[i]);
  }
  return out;
}

const char* c_bold() {
  return use_color() ? "\033[1m" : "";
}
const char* c_dim() {
  return use_color() ? "\033[2m" : "";
}
const char* c_green() {
  return use_color() ? "\033[32m" : "";
}
const char* c_reset() {
  return use_color() ? "\033[0m" : "";
}

struct RawMode {
  termios saved{};
  bool active = false;
  RawMode() {
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
      return;
    }
    termios raw = saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    active = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
  }
  ~RawMode() {
    if (active) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    }
  }
  RawMode(const RawMode&) = delete;
  RawMode& operator=(const RawMode&) = delete;
};

int read_byte() {
  unsigned char b = 0;
  return ::read(STDIN_FILENO, &b, 1) == 1 ? b : -1;
}

// Minimal line editing on a terminal: arrow-key history, cursor movement,
// ctrl-a/e/u. Falls back to plain getline when stdin is piped, so scripted
// input behaves exactly as before.
class LineEditor {
public:
  std::optional<std::string> read(const char* prompt) {
    if (isatty(STDIN_FILENO) != 1) {
      std::fputs(prompt, stdout);
      std::fflush(stdout);
      std::string line;
      if (!std::getline(std::cin, line)) {
        return std::nullopt;
      }
      return line;
    }

    const RawMode raw;
    std::string buf;
    std::string draft;
    std::size_t cursor = 0;
    std::size_t hist = history_.size();

    const auto redraw = [&] {
      std::printf("\r\033[K%s%s", prompt, buf.c_str());
      if (cursor < buf.size()) {
        std::printf("\033[%zuD", buf.size() - cursor);
      }
      std::fflush(stdout);
    };
    redraw();

    while (true) {
      const int c = read_byte();
      if (c < 0 || c == 4) { // EOF, or ctrl-d on an empty line
        if (c == 4 && !buf.empty()) {
          continue;
        }
        std::printf("\n");
        return std::nullopt;
      }
      if (c == '\r' || c == '\n') {
        std::printf("\n");
        if (!buf.empty() && (history_.empty() || history_.back() != buf)) {
          history_.push_back(buf);
        }
        return buf;
      }
      if (c == 3) { // ctrl-c: drop the line
        std::printf("^C\n");
        buf.clear();
        draft.clear();
        cursor = 0;
        hist = history_.size();
        redraw();
        continue;
      }
      if (c == 127 || c == 8) { // backspace
        if (cursor > 0) {
          buf.erase(cursor - 1, 1);
          --cursor;
          redraw();
        }
        continue;
      }
      if (c == 21) { // ctrl-u
        buf.clear();
        cursor = 0;
        redraw();
        continue;
      }
      if (c == 1) { // ctrl-a
        cursor = 0;
        redraw();
        continue;
      }
      if (c == 5) { // ctrl-e
        cursor = buf.size();
        redraw();
        continue;
      }
      if (c == 27) { // escape sequence
        if (read_byte() != '[') {
          continue;
        }
        switch (read_byte()) {
        case 'A': // up: back through history, keeping the unfinished line
          if (hist > 0) {
            if (hist == history_.size()) {
              draft = buf;
            }
            --hist;
            buf = history_[hist];
            cursor = buf.size();
            redraw();
          }
          break;
        case 'B': // down
          if (hist < history_.size()) {
            ++hist;
            buf = hist == history_.size() ? draft : history_[hist];
            cursor = buf.size();
            redraw();
          }
          break;
        case 'C':
          if (cursor < buf.size()) {
            ++cursor;
            redraw();
          }
          break;
        case 'D':
          if (cursor > 0) {
            --cursor;
            redraw();
          }
          break;
        case 'H':
          cursor = 0;
          redraw();
          break;
        case 'F':
          cursor = buf.size();
          redraw();
          break;
        case '3': // delete key
          if (read_byte() == '~' && cursor < buf.size()) {
            buf.erase(cursor, 1);
            redraw();
          }
          break;
        default:
          break;
        }
        continue;
      }
      if (c >= 32 && c < 127) {
        buf.insert(buf.begin() + static_cast<std::string::difference_type>(cursor),
                   static_cast<char>(c));
        ++cursor;
        redraw();
      }
    }
  }

private:
  std::vector<std::string> history_;
};

Chips pot_size(const State& s) {
  Chips pot = 0;
  for (int i = 0; i < s.num_seats; ++i) {
    pot += s.total_committed[i];
  }
  return pot;
}

// Even seats play call-station, odd seats mix it up.
Action bot_action(const State& s, const LegalActions& la, Rng& rng) {
  if (s.to_act % 2 == 0) {
    return {la.can_check ? Action::Kind::kCheck : Action::Kind::kCall, 0};
  }
  const std::uint32_t roll = rng.below(10);
  if (roll == 0 && la.can_call) {
    return {Action::Kind::kFold, 0};
  }
  if (roll >= 7 && la.can_raise) {
    const auto spread = static_cast<std::uint32_t>(la.max_raise_to - la.min_raise_to + 1);
    return {Action::Kind::kRaise, la.min_raise_to + static_cast<Chips>(rng.below(spread))};
  }
  return {la.can_check ? Action::Kind::kCheck : Action::Kind::kCall, 0};
}

int cmd_eval(const std::vector<std::string_view>& args) {
  bool json = false;
  std::string text;
  for (const auto arg : args) {
    if (arg == "--json") {
      json = true;
    } else {
      text += arg;
      text += ' ';
    }
  }
  const auto cards = parse_cards(text);
  if (!cards || cards->size() < 5 || cards->size() > 7) {
    std::fputs("expected 5 to 7 distinct cards, e.g. poker eval \"AhKd 7c8c9h\"\n", stderr);
    return 2;
  }
  const eval::HandRank rank = eval::evaluate_fast(*cards);
  const auto name = eval::category_name(eval::category(rank));
  if (json) {
    std::printf("{\"category\":\"%.*s\",\"rank\":%u}\n", static_cast<int>(name.size()), name.data(),
                rank);
  } else {
    std::printf("%.*s (rank %u)\n", static_cast<int>(name.size()), name.data(), rank);
  }
  return 0;
}

int cmd_sim(const std::vector<std::string_view>& args) {
  std::uint64_t hands = 10000;
  std::uint64_t seed = 0;
  std::uint64_t seats = 5;
  bool json = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto arg = args[i];
    const auto next = [&]() -> std::optional<std::uint64_t> {
      return i + 1 < args.size() ? parse_u64(args[++i]) : std::nullopt;
    };
    if (arg == "--json") {
      json = true;
    } else if (arg == "--hands") {
      if (const auto v = next()) {
        hands = *v;
      } else {
        std::fputs("--hands wants a number\n", stderr);
        return 2;
      }
    } else if (arg == "--seed") {
      if (const auto v = next()) {
        seed = *v;
      } else {
        std::fputs("--seed wants a number\n", stderr);
        return 2;
      }
    } else if (arg == "--bots") {
      const auto v = next();
      if (!v || *v < 2 || *v > kMaxSeats) {
        std::fprintf(stderr, "--bots wants 2 to %d\n", kMaxSeats);
        return 2;
      }
      seats = *v;
    } else {
      std::fprintf(stderr, "unknown option '%.*s'\n", static_cast<int>(arg.size()), arg.data());
      return 2;
    }
  }

  const TableConfig cfg{5, 10};
  const int n = static_cast<int>(seats);
  Rng rng{seed};
  std::uint64_t showdowns = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t hand = 0; hand < hands; ++hand) {
    const std::vector<Chips> stacks(static_cast<std::size_t>(n), 100 * cfg.big_blind);
    const auto deck = shuffled_deck(rng);
    State s = new_hand(cfg, stacks, static_cast<int>(hand % seats), deck);
    while (!is_terminal(s)) {
      apply(s, bot_action(s, legal_actions(s), rng));
    }
    showdowns += s.board_count == kBoardCards ? 1 : 0;
    payouts(s);
  }
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
  const double rate = elapsed.count() > 0 ? static_cast<double>(hands) / elapsed.count() : 0;

  if (json) {
    std::printf("{\"hands\":%llu,\"showdowns\":%llu,\"seconds\":%.3f,\"hands_per_sec\":%.0f}\n",
                static_cast<unsigned long long>(hands), static_cast<unsigned long long>(showdowns),
                elapsed.count(), rate);
  } else {
    std::printf("%llu hands, %d seats, seed %llu\n", static_cast<unsigned long long>(hands), n,
                static_cast<unsigned long long>(seed));
    std::printf("%llu showdowns\n", static_cast<unsigned long long>(showdowns));
    std::printf("%.3fs, %.0f hands/s\n", elapsed.count(), rate);
  }
  return 0;
}

void print_table_help() {
  std::fputs("  fold  (f)             Fold your hand. Legal whenever it is your turn.\n"
             "  check (x)             Pass the action when nothing is owed.\n"
             "  call  (c)             Call the current bet, all-in for less if it covers you.\n"
             "  raise (r, bet, b) <n> Bet or raise to a total of n chips, or 'allin'.\n"
             "  allin (shove, jam)    Move all-in.\n"
             "  help  (h, ?)          This list.\n"
             "  quit  (exit, q)       Leave the table.\n",
             stdout);
}

struct Session {
  std::vector<std::string> names;
  std::vector<Chips> stacks;
  TableConfig cfg;
  std::uint64_t hands_played = 0;
  LineEditor editor;
};

const char* street_name(Street st) {
  static const char* kStreets[] = {"preflop", "flop", "turn", "river"};
  return kStreets[static_cast<int>(st)];
}

// "bot3 990", "bot1 folded", "bot5 all-in 250"
std::string seat_status(const State& s, const Session& t, int seat) {
  std::string out = t.names[static_cast<std::size_t>(seat)];
  if (s.folded[seat]) {
    out += " folded";
  } else if (s.all_in[seat]) {
    out += " all-in " + std::to_string(s.total_committed[seat]);
  } else {
    out += " " + std::to_string(s.stacks[seat]);
  }
  return out;
}

void show_street(const State& s) {
  std::printf("\n  %s── %s%s  %s %s· pot %lld%s\n", c_bold(), street_name(s.street), c_reset(),
              cards_str(s.board, s.board_count).c_str(), c_dim(),
              static_cast<long long>(pot_size(s)), c_reset());
}

void show_turn(const State& s, const Session& t, const LegalActions& la) {
  std::printf("  %syou %s%s · stack %lld · pot %lld", c_bold(),
              cards_str(s.hole[0], kHoleCards).c_str(), c_reset(),
              static_cast<long long>(s.stacks[0]), static_cast<long long>(pot_size(s)));
  if (la.can_call) {
    std::printf(" · %sto call %lld%s", c_bold(), static_cast<long long>(la.call_cost), c_reset());
  }
  std::printf("\n  ");
  for (int i = 1; i < s.num_seats; ++i) {
    std::printf("%s%s", i > 1 ? " · " : "", seat_status(s, t, i).c_str());
  }
  std::printf("\n  %s", c_dim());
  std::printf("fold");
  if (la.can_check) {
    std::printf(" · check");
  }
  if (la.can_call) {
    std::printf(" · call %lld", static_cast<long long>(la.call_cost));
  }
  if (la.can_raise) {
    std::printf(" · raise to %lld-%lld", static_cast<long long>(la.min_raise_to),
                static_cast<long long>(la.max_raise_to));
  }
  std::printf(" · help%s\n", c_reset());
}

// Reads until the line is a legal action; empty optional means quit.
std::optional<Action> human_action(const State& s, const LegalActions& la, Session& t) {
  show_turn(s, t, la);
  while (true) {
    const auto line = t.editor.read("> ");
    if (!line) {
      return std::nullopt; // EOF or ctrl-d: leave the table
    }
    std::string word;
    std::string amount_text;
    std::istringstream in{*line};
    in >> word >> amount_text;

    if (word.empty()) {
      continue;
    }
    if (word == "help" || word == "h" || word == "?") {
      print_table_help();
      continue;
    }
    if (word == "quit" || word == "exit" || word == "q") {
      return std::nullopt;
    }
    if (word == "fold" || word == "f") {
      return Action{Action::Kind::kFold, 0};
    }
    if (word == "check" || word == "x") {
      if (!la.can_check) {
        std::printf("  there is a bet to you; call, raise or fold\n");
        continue;
      }
      return Action{Action::Kind::kCheck, 0};
    }
    if (word == "call" || word == "c") {
      if (!la.can_call) {
        std::printf("  nothing to call; you can check\n");
        continue;
      }
      return Action{Action::Kind::kCall, 0};
    }
    if (word == "allin" || word == "shove" || word == "jam") {
      if (la.can_raise) {
        return Action{Action::Kind::kRaise, la.max_raise_to};
      }
      if (la.can_call) {
        return Action{Action::Kind::kCall, 0};
      }
      std::printf("  you cannot put more chips in right now\n");
      continue;
    }
    if (word == "raise" || word == "r" || word == "bet" || word == "b") {
      if (!la.can_raise) {
        std::printf("  you cannot raise right now\n");
        continue;
      }
      if (amount_text == "allin") {
        return Action{Action::Kind::kRaise, la.max_raise_to};
      }
      const auto amount = parse_chips(amount_text);
      if (!amount) {
        std::printf("  raise to how much? e.g. 'raise 60' or 'raise allin'\n");
        continue;
      }
      if (*amount != la.max_raise_to && (*amount < la.min_raise_to || *amount > la.max_raise_to)) {
        std::printf("  raise total must be %lld to %lld\n", static_cast<long long>(la.min_raise_to),
                    static_cast<long long>(la.max_raise_to));
        continue;
      }
      return Action{Action::Kind::kRaise, *amount};
    }
    std::printf("  unknown command '%s', try help\n", word.c_str());
  }
}

void announce(const State& s, const Session& t, int seat, Action a) {
  const char* name = t.names[static_cast<std::size_t>(seat)].c_str();
  const bool you = seat == 0; // second person for the human seat
  switch (a.kind) {
  case Action::Kind::kFold:
    std::printf("%s %s\n", name, you ? "fold" : "folds");
    break;
  case Action::Kind::kCheck:
    std::printf("%s %s\n", name, you ? "check" : "checks");
    break;
  case Action::Kind::kCall:
    std::printf(
        "%s %s %lld\n", name, you ? "call" : "calls",
        static_cast<long long>(std::min(s.current_bet - s.street_committed[seat], s.stacks[seat])));
    break;
  case Action::Kind::kRaise: {
    const char* verb =
        s.current_bet == 0 ? (you ? "bet" : "bets") : (you ? "raise to" : "raises to");
    std::printf("%s %s %lld%s\n", name, verb, static_cast<long long>(a.amount),
                a.amount == s.street_committed[seat] + s.stacks[seat] ? " (all-in)" : "");
    break;
  }
  }
}

void show_result(const State& s, Session& t) {
  const auto pay = payouts(s);
  int alive = 0;
  for (int i = 0; i < s.num_seats; ++i) {
    alive += s.folded[i] ? 0 : 1;
  }
  if (alive > 1) {
    std::printf("board: %s\n", cards_str(s.board, s.board_count).c_str());
    for (int i = 0; i < s.num_seats; ++i) {
      if (s.folded[i]) {
        continue;
      }
      Card seven[kHoleCards + kBoardCards];
      seven[0] = s.hole[i][0];
      seven[1] = s.hole[i][1];
      for (int b = 0; b < kBoardCards; ++b) {
        seven[kHoleCards + b] = s.board[b];
      }
      const auto name = eval::category_name(eval::category(eval::evaluate_fast(seven)));
      std::printf("%s shows %s (%.*s)\n", t.names[static_cast<std::size_t>(i)].c_str(),
                  cards_str(s.hole[i], kHoleCards).c_str(), static_cast<int>(name.size()),
                  name.data());
    }
  }
  for (int i = 0; i < s.num_seats; ++i) {
    const Chips won = pay[static_cast<std::size_t>(i)];
    t.stacks[static_cast<std::size_t>(i)] = s.stacks[i] + won;
    if (won > 0) {
      std::printf("%s wins %lld\n", t.names[static_cast<std::size_t>(i)].c_str(),
                  static_cast<long long>(won));
    }
  }
}

int cmd_play(const std::vector<std::string_view>& args) {
  int bots = 5;
  TableConfig cfg{5, 10};
  std::optional<std::uint64_t> seed;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto arg = args[i];
    const auto next = [&]() -> std::optional<std::string_view> {
      return i + 1 < args.size() ? std::optional{args[++i]} : std::nullopt;
    };
    if (arg == "--bots") {
      const auto v = next();
      const auto n = v ? parse_u64(*v) : std::nullopt;
      if (!n || *n < 1 || *n > kMaxSeats - 1) {
        std::fprintf(stderr, "--bots wants 1 to %d\n", kMaxSeats - 1);
        return 2;
      }
      bots = static_cast<int>(*n);
    } else if (arg == "--stakes") {
      const auto v = next();
      const auto stakes = v ? parse_stakes(*v) : std::nullopt;
      if (!stakes) {
        std::fputs("--stakes wants sb/bb, e.g. 5/10\n", stderr);
        return 2;
      }
      cfg = *stakes;
    } else if (arg == "--seed") {
      const auto v = next();
      seed = v ? parse_u64(*v) : std::nullopt;
      if (!seed) {
        std::fputs("--seed wants a number\n", stderr);
        return 2;
      }
    } else {
      std::fprintf(stderr, "unknown option '%.*s'\n", static_cast<int>(arg.size()), arg.data());
      return 2;
    }
  }

  if (!seed) {
    std::random_device rd;
    seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  }
  Rng rng{*seed};

  const int seats = bots + 1;
  const Chips buy_in = 100 * cfg.big_blind;
  Session t;
  t.cfg = cfg;
  t.names.emplace_back("you");
  for (int i = 1; i < seats; ++i) {
    t.names.push_back("bot" + std::to_string(i));
  }
  t.stacks.assign(static_cast<std::size_t>(seats), buy_in);

  std::printf("no-limit hold'em, blinds %lld/%lld, %d bots, stacks %lld\n",
              static_cast<long long>(cfg.small_blind), static_cast<long long>(cfg.big_blind), bots,
              static_cast<long long>(buy_in));
  std::printf("type help for the commands\n");

  for (std::uint64_t hand_no = 0;; ++hand_no) {
    if (t.stacks[0] <= 0) {
      std::printf("you're bust after %llu hands\n",
                  static_cast<unsigned long long>(t.hands_played));
      return 0;
    }
    for (int i = 1; i < seats; ++i) {
      if (t.stacks[static_cast<std::size_t>(i)] <= 0) {
        t.stacks[static_cast<std::size_t>(i)] = buy_in;
        std::printf("%s re-buys\n", t.names[static_cast<std::size_t>(i)].c_str());
      }
    }

    const int button = static_cast<int>(hand_no % static_cast<std::uint64_t>(seats));
    std::printf("\n=== hand #%llu   button: %s\n", static_cast<unsigned long long>(hand_no + 1),
                t.names[static_cast<std::size_t>(button)].c_str());
    const auto deck = shuffled_deck(rng);
    State s = new_hand(cfg, t.stacks, button, deck);
    ++t.hands_played;

    int shown_board = 0;
    while (!is_terminal(s)) {
      const int seat = s.to_act;
      const LegalActions la = legal_actions(s);
      if (seat == 0) {
        const auto a = human_action(s, la, t);
        if (!a) {
          std::printf("you leave with %lld chips after %llu hands\n",
                      static_cast<long long>(s.stacks[0]),
                      static_cast<unsigned long long>(t.hands_played));
          return 0;
        }
        shown_board = s.board_count;
        announce(s, t, seat, *a);
        apply(s, *a);
      } else {
        if (s.board_count > shown_board) {
          static const char* kStreets[] = {"preflop", "flop", "turn", "river"};
          std::printf("-- %s: %s    pot %lld\n", kStreets[static_cast<int>(s.street)],
                      cards_str(s.board, s.board_count).c_str(),
                      static_cast<long long>(pot_size(s)));
        }
        shown_board = s.board_count;
        const Action a = bot_action(s, la, rng);
        announce(s, t, seat, a);
        apply(s, a);
      }
    }
    show_result(s, t);
  }
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  for (int i = 2; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  const std::string_view cmd = argc > 1 ? argv[1] : "";
  if (cmd == "eval") {
    return cmd_eval(args);
  }
  if (cmd == "sim") {
    return cmd_sim(args);
  }
  if (cmd == "play") {
    return cmd_play(args);
  }
  if (!cmd.empty()) {
    std::fprintf(stderr, "unknown command '%.*s'\n\n", static_cast<int>(cmd.size()), cmd.data());
  }
  print_usage();
  return 2;
}

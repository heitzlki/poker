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

#include <ctime>
#include <sys/ioctl.h>
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
const char* c_red() {
  return use_color() ? "\033[31m" : "";
}
const char* c_yellow() {
  return use_color() ? "\033[33m" : "";
}
const char* c_cyan() {
  return use_color() ? "\033[36m" : "";
}
const char* c_blue() {
  return use_color() ? "\033[34m" : "";
}
const char* c_magenta() {
  return use_color() ? "\033[35m" : "";
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
  RawMode(RawMode&&) = delete;
  RawMode& operator=(const RawMode&) = delete;
  RawMode& operator=(RawMode&&) = delete;
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
      if (c == 3) { // ctrl-c: leave the table, same as quit
        std::printf("^C\n");
        return std::nullopt;
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

// Returns the number of lines printed, so the transient block can be erased.
int print_table_help() {
  std::fputs("  fold  (f)             Fold your hand. Legal whenever it is your turn.\n"
             "  check (x)             Pass the action when nothing is owed.\n"
             "  call  (c)             Call the current bet, all-in for less if it covers you.\n"
             "  raise (r, bet, b) <n> Bet or raise to a total of n chips, or 'allin'.\n"
             "  allin (shove, jam)    Move all-in.\n"
             "  help  (h, ?)          This list.\n"
             "  quit  (exit, q)       Leave the table.\n",
             stdout);
  return 7;
}

// Moves the cursor up over the last n lines and clears them, so the state
// panel and prompt vanish from the transcript once an action is chosen and
// only the log remains. No-op when output is piped.
void erase_lines(int n) {
  if (n > 0 && isatty(STDOUT_FILENO) == 1) {
    std::printf("\033[%dA\033[0J", n);
    std::fflush(stdout);
  }
}

struct Session {
  std::vector<std::string> names;
  std::vector<Chips> stacks;
  TableConfig cfg;
  std::uint64_t hands_played = 0;
  LineEditor editor;
  bool tui = false;             // fixed panel + scrolling log, when on a terminal
  std::vector<std::string> log; // timestamped history, newest last
};

std::string timestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
  return buf;
}

// A history line: appended to the log in tui mode, printed directly otherwise.
void log_event(Session& t, const std::string& text) {
  if (t.tui) {
    t.log.push_back(timestamp() + "  " + text);
  } else {
    std::printf("  %s\n", text.c_str());
  }
}

int term_rows() {
  winsize w{};
  return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 ? w.ws_row : 24;
}

int term_cols() {
  winsize w{};
  return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 ? w.ws_col : 80;
}

std::string hrule(int cols) {
  std::string out;
  for (int i = 0; i < cols; ++i) {
    out += "─";
  }
  return out;
}

std::string pad_right(const std::string& s, std::size_t width) {
  return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
}

// Your name in bold blue, bots in magenta, everywhere a name shows up.
// Padding happens before coloring: escape codes have no width.
std::string seat_name(const Session& t, int seat, std::size_t pad = 0) {
  const std::string padded = pad_right(t.names[static_cast<std::size_t>(seat)], pad);
  if (seat == 0) {
    return std::string(c_bold()) + c_blue() + padded + c_reset();
  }
  return std::string(c_magenta()) + padded + c_reset();
}

// "[you]  " for log lines, padded so the actions line up in a column.
std::string seat_tag(const Session& t, int seat, std::size_t pad = 7) {
  const std::string padded = pad_right("[" + t.names[static_cast<std::size_t>(seat)] + "]", pad);
  if (seat == 0) {
    return std::string(c_bold()) + c_blue() + padded + c_reset();
  }
  return std::string(c_magenta()) + padded + c_reset();
}

const char* street_name(Street st) {
  static const char* kStreets[] = {"PREFLOP", "FLOP", "TURN", "RIVER"};
  return kStreets[static_cast<int>(st)];
}

void show_street(const State& s) {
  std::printf("\n  %s── %s ──%s  %s  %s· pot %lld%s\n", c_bold(), street_name(s.street), c_reset(),
              cards_str(s.board, s.board_count).c_str(), c_dim(),
              static_cast<long long>(pot_size(s)), c_reset());
}

// One aligned row per seat: marker, name, stack, street bet, status/cards.
void seat_row(const State& s, const Session& t, int seat) {
  const bool you = seat == 0;
  std::printf("  %s", seat_name(t, seat, 5).c_str());
  if (s.folded[seat]) {
    std::printf("  %s%6s%s", c_dim(), "-", c_reset());
    std::printf("          %sfolded%s", c_dim(), c_reset());
  } else {
    std::printf("  %6lld%s", static_cast<long long>(s.stacks[seat]), you ? c_reset() : "");
    if (s.street_committed[seat] > 0) {
      std::printf("  %sbet %-5lld%s", c_yellow(), static_cast<long long>(s.street_committed[seat]),
                  c_reset());
    } else {
      std::printf("  %9s", "");
    }
    if (s.all_in[seat]) {
      std::printf(" %sall-in%s", c_red(), c_reset());
    }
  }
  if (you) {
    std::printf("  %s", cards_str(s.hole[0], kHoleCards).c_str());
  }
  std::printf("%s\n", c_reset());
}

void show_turn(const State& s, const Session& t, const LegalActions& la) {
  std::printf("\n");
  for (int i = 0; i < s.num_seats; ++i) {
    seat_row(s, t, i);
  }
  std::printf("  pot %lld", static_cast<long long>(pot_size(s)));
  if (la.can_call) {
    std::printf(" · %sto call %lld%s", c_bold(), static_cast<long long>(la.call_cost), c_reset());
  }
  std::printf("\n");
  std::printf("  %s(f)old%s", c_red(), c_reset());
  if (la.can_check) {
    std::printf(" · %s(x) check%s", c_dim(), c_reset());
  }
  if (la.can_call) {
    std::printf(" · %s(c)all %lld%s", c_yellow(), static_cast<long long>(la.call_cost), c_reset());
  }
  if (la.can_raise) {
    std::printf(" · %s(r)aise to %lld-%lld%s", c_cyan(), static_cast<long long>(la.min_raise_to),
                static_cast<long long>(la.max_raise_to), c_reset());
  }
  std::printf(" · %s(h)elp%s\n", c_dim(), c_reset());
}

// "  1 you       990  in 30   8♦ 6♣   ◀ to act": grey stack, green share of
// the pot this hand, then the cards.
// The "in N" / "pot N" column is sized to the widest value on screen so the
// cards never shift, however big the pot gets.
std::size_t money_width(const State& s) {
  std::size_t w = 9;
  for (int i = 0; i < s.num_seats; ++i) {
    w = std::max(w, ("in " + std::to_string(s.total_committed[i])).size() + 2);
  }
  w = std::max(w, ("pot " + std::to_string(pot_size(s))).size() + 2);
  return w;
}

std::string tui_seat_row(const State& s, const Session& t, int seat, std::size_t money_w) {
  const bool you = seat == 0;
  std::string row = "   " + std::to_string(seat + 1) + " ";
  row += seat_name(t, seat, 7);
  if (s.folded[seat]) {
    row += std::string(c_dim()) + "     -   folded" + c_reset();
  } else {
    char num[24];
    std::snprintf(num, sizeof num, "%6lld", static_cast<long long>(s.stacks[seat]));
    row += std::string(c_dim()) + num + c_reset();
    const std::string in_pot =
        s.total_committed[seat] > 0 ? "in " + std::to_string(s.total_committed[seat]) : "";
    row += "  " + std::string(c_green()) + pad_right(in_pot, money_w) + c_reset();
    row += you ? cards_str(s.hole[0], kHoleCards) : std::string(c_dim()) + "?? ??" + c_reset();
    if (s.all_in[seat]) {
      row += std::string("   ") + c_red() + "all-in" + c_reset();
    }
  }
  if (s.to_act == seat) {
    row += std::string("   ") + c_bold() + "◀ to act" + c_reset();
  }
  return row;
}

// Options bar at the bottom of the frame; null means "between hands".
std::string options_text(const LegalActions* la) {
  std::string out = " ";
  if (la == nullptr) {
    return out + c_dim() + "[enter] deal the next hand   [q]uit" + c_reset();
  }
  out += std::string(c_red()) + "[f]old" + c_reset();
  if (la->can_check) {
    out += std::string("  ") + c_dim() + "[x]check" + c_reset();
  }
  if (la->can_call) {
    out += std::string("  ") + c_yellow() + "[c]all " + std::to_string(la->call_cost) + c_reset();
  }
  if (la->can_raise) {
    out += std::string("  ") + c_cyan() + "[r]aise " + std::to_string(la->min_raise_to) + ".." +
           std::to_string(la->max_raise_to) + c_reset();
    out += std::string("  ") + c_red() + "[a]ll-in" + c_reset();
  }
  out += std::string("  ") + c_dim() + "[?]help  [q]uit";
  if (la->can_check) {
    out += "  enter checks";
  } else if (la->can_call) {
    out += "  enter calls " + std::to_string(la->call_cost);
  }
  out += c_reset();
  return out;
}

// Repaint the whole screen: status panel on top, log in the middle, options
// bar above the prompt line. Overwrites in place, so nothing scrolls except
// the log itself.
void draw_frame(const State& s, Session& t, const LegalActions* la) {
  const int rows = term_rows();
  const int cols = term_cols();
  std::string out = "\033[H";
  const auto line = [&](const std::string& content) {
    out += content;
    out += "\033[K\n";
  };

  line(std::string(c_bold()) + "  POKER · NLHE " + std::to_string(t.cfg.small_blind) + "/" +
       std::to_string(t.cfg.big_blind) + " · " + std::to_string(s.num_seats) + " seats" +
       c_reset());
  line(std::string(c_dim()) + hrule(cols) + c_reset());
  for (int i = 0; i < s.num_seats; ++i) {
    line(tui_seat_row(s, t, i, money_width(s)));
  }
  // One column left of the "in" labels, so the amounts line up digit for
  // digit; the board still starts under the hole cards.
  std::string board(19, ' ');
  board += pad_right("pot " + std::to_string(pot_size(s)), money_width(s) + 1);
  for (int i = 0; i < kBoardCards; ++i) {
    if (i > 0) {
      board += ' ';
    }
    board += i < s.board_count ? card_display(s.board[i]) : std::string(c_dim()) + "·" + c_reset();
  }
  line(board);
  line(std::string(c_dim()) + hrule(cols) + c_reset());

  const int header = s.num_seats + 4;
  const int avail = std::max(rows - header - 2, 1);
  const int total = static_cast<int>(t.log.size());
  const int start = total > avail ? total - avail : 0;
  for (int i = start; i < total; ++i) {
    line(" " + t.log[static_cast<std::size_t>(i)]);
  }
  for (int i = total - start; i < avail; ++i) {
    line("");
  }
  line(options_text(la));
  out += "\033[K";
  std::fputs(out.c_str(), stdout);
  std::fflush(stdout);
}

struct Parsed {
  enum class What : std::uint8_t { kEmpty, kAction, kQuit, kHelp, kError };
  What what = What::kEmpty;
  Action action{};
  std::string error;
};

Parsed parse_table_command(const std::string& input, const LegalActions& la) {
  std::string word;
  std::string amount_text;
  std::istringstream in{input};
  in >> word >> amount_text;

  Parsed p;
  const auto act = [&](Action a) { p = {Parsed::What::kAction, a, {}}; };
  const auto err = [&](std::string e) { p = {Parsed::What::kError, {}, std::move(e)}; };

  if (word.empty()) { // bare enter takes the free/cheap continue
    if (la.can_check) {
      act({Action::Kind::kCheck, 0});
    } else if (la.can_call) {
      act({Action::Kind::kCall, 0});
    }
    return p;
  }
  if (word == "help" || word == "h" || word == "?") {
    p.what = Parsed::What::kHelp;
  } else if (word == "quit" || word == "exit" || word == "q") {
    p.what = Parsed::What::kQuit;
  } else if (word == "fold" || word == "f") {
    act({Action::Kind::kFold, 0});
  } else if (word == "check" || word == "x") {
    if (!la.can_check) {
      err("there is a bet to you; call, raise or fold");
    } else {
      act({Action::Kind::kCheck, 0});
    }
  } else if (word == "call" || word == "c") {
    if (!la.can_call) {
      err("nothing to call; you can check");
    } else {
      act({Action::Kind::kCall, 0});
    }
  } else if (word == "allin" || word == "a" || word == "shove" || word == "jam") {
    if (la.can_raise) {
      act({Action::Kind::kRaise, la.max_raise_to});
    } else if (la.can_call) {
      act({Action::Kind::kCall, 0});
    } else {
      err("you cannot put more chips in right now");
    }
  } else if (word == "raise" || word == "r" || word == "bet" || word == "b") {
    if (!la.can_raise) {
      err("you cannot raise right now");
    } else if (amount_text == "allin") {
      act({Action::Kind::kRaise, la.max_raise_to});
    } else {
      const auto amount = parse_chips(amount_text);
      if (!amount) {
        err("raise to how much? e.g. 'raise 60' or 'raise allin'");
      } else if (*amount != la.max_raise_to &&
                 (*amount < la.min_raise_to || *amount > la.max_raise_to)) {
        err("raise total must be " + std::to_string(la.min_raise_to) + " to " +
            std::to_string(la.max_raise_to));
      } else {
        act({Action::Kind::kRaise, *amount});
      }
    }
  } else {
    err("unknown command '" + word + "', try help");
  }
  return p;
}

// Reads until the line is a legal action; empty optional means quit.
std::optional<Action> human_action(const State& s, const LegalActions& la, Session& t) {
  if (t.tui) {
    while (true) {
      draw_frame(s, t, &la);
      const auto line = t.editor.read(" > ");
      if (!line) {
        return std::nullopt;
      }
      const Parsed p = parse_table_command(*line, la);
      if (p.what == Parsed::What::kAction) {
        return p.action;
      }
      if (p.what == Parsed::What::kQuit) {
        return std::nullopt;
      }
      if (p.what == Parsed::What::kHelp) {
        log_event(t, "commands: fold f · check x · call c · raise r/b <n|allin> · allin a "
                     "· quit q");
      } else if (p.what == Parsed::What::kError) {
        log_event(t, p.error);
      }
    }
  }

  show_turn(s, t, la);
  int lines = s.num_seats + 3; // blank line, seat rows, pot line, options line
  std::optional<Action> chosen;

  while (!chosen) {
    const auto line = t.editor.read("> ");
    if (!line) {
      return std::nullopt; // quit via ctrl-c, ctrl-d or EOF: keep the panel
    }
    ++lines; // the submitted prompt line
    const Parsed p = parse_table_command(*line, la);
    if (p.what == Parsed::What::kAction) {
      chosen = p.action;
    } else if (p.what == Parsed::What::kQuit) {
      return std::nullopt;
    } else if (p.what == Parsed::What::kHelp) {
      lines += print_table_help();
    } else if (p.what == Parsed::What::kError) {
      std::printf("  %s\n", p.error.c_str());
      ++lines;
    }
  }

  erase_lines(lines); // the panel was state, not history; only the action stays
  return chosen;
}

// Aligned action log with one color per action kind, so a glance down the
// column shows what happened: red folds, dim checks, yellow calls, cyan raises.
std::string action_text(const State& s, const Session& t, int seat, Action a) {
  const bool you = seat == 0;
  std::string out = seat_tag(t, seat) + " ";
  switch (a.kind) {
  case Action::Kind::kFold:
    out += std::string(c_red()) + (you ? "fold" : "folds") + c_reset();
    break;
  case Action::Kind::kCheck:
    out += std::string(c_dim()) + (you ? "check" : "checks") + c_reset();
    break;
  case Action::Kind::kCall:
    out += std::string(c_yellow()) + (you ? "call  " : "calls ") +
           std::to_string(std::min(s.current_bet - s.street_committed[seat], s.stacks[seat])) +
           c_reset();
    break;
  case Action::Kind::kRaise:
    out += std::string(c_cyan()) +
           (s.current_bet == 0 ? (you ? "bet  " : "bets ") : (you ? "raise to  " : "raises to ")) +
           std::to_string(a.amount) + c_reset();
    if (a.amount == s.street_committed[seat] + s.stacks[seat]) {
      out += std::string(" ") + c_red() + "all-in" + c_reset();
    }
    break;
  }
  return out;
}

void announce(const State& s, Session& t, int seat, Action a) {
  log_event(t, action_text(s, t, seat, a));
}

void show_result(const State& s, Session& t) {
  const auto pay = payouts(s);
  int alive = 0;
  for (int i = 0; i < s.num_seats; ++i) {
    alive += s.folded[i] ? 0 : 1;
  }
  if (alive > 1) {
    if (!t.tui) {
      std::printf("\n  %s── SHOWDOWN ──%s  %s  %s· pot %lld%s\n", c_bold(), c_reset(),
                  cards_str(s.board, s.board_count).c_str(), c_dim(),
                  static_cast<long long>(pot_size(s)), c_reset());
    }
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
      log_event(t, seat_tag(t, i) + " " + (i == 0 ? "show  " : "shows ") +
                       cards_str(s.hole[i], kHoleCards) + " — " + c_dim() + std::string(name) +
                       c_reset());
    }
  }
  for (int i = 0; i < s.num_seats; ++i) {
    const Chips won = pay[static_cast<std::size_t>(i)];
    t.stacks[static_cast<std::size_t>(i)] = s.stacks[i] + won;
    if (won > 0) {
      log_event(t, seat_tag(t, i) + " " + c_green() + (i == 0 ? "win  " : "wins ") +
                       std::to_string(won) + (alive == 1 ? " uncontested" : "") + c_reset());
    }
  }
  if (!t.tui) {
    std::printf("  %sstacks  ", c_dim());
    for (int i = 0; i < s.num_seats; ++i) {
      std::printf("%s%s %lld", i > 0 ? " · " : "", t.names[static_cast<std::size_t>(i)].c_str(),
                  static_cast<long long>(t.stacks[static_cast<std::size_t>(i)]));
    }
    std::printf("%s\n", c_reset());
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

  t.tui = isatty(STDOUT_FILENO) == 1 && isatty(STDIN_FILENO) == 1;
  const auto leave_tui = [&t] {
    if (t.tui) {
      std::fputs("\033[?1049l", stdout); // back to the normal screen buffer
      t.tui = false;
    }
  };
  const auto farewell = [&](Chips chips) {
    leave_tui();
    std::printf("%syou leave with %lld chips after %llu hands%s\n", c_bold(),
                static_cast<long long>(chips), static_cast<unsigned long long>(t.hands_played),
                c_reset());
  };

  if (t.tui) {
    std::fputs("\033[?1049h\033[H\033[2J", stdout);
  } else {
    std::printf("%sno-limit hold'em%s · blinds %lld/%lld · %d bot%s · stacks %lld\n", c_bold(),
                c_reset(), static_cast<long long>(cfg.small_blind),
                static_cast<long long>(cfg.big_blind), bots, bots == 1 ? "" : "s",
                static_cast<long long>(buy_in));
    std::printf("%stype help at the table for the commands%s\n", c_dim(), c_reset());
  }

  for (std::uint64_t hand_no = 0;; ++hand_no) {
    if (t.stacks[0] <= 0) {
      leave_tui();
      std::printf("\n%syou're bust after %llu hands%s\n", c_bold(),
                  static_cast<unsigned long long>(t.hands_played), c_reset());
      return 0;
    }
    for (int i = 1; i < seats; ++i) {
      if (t.stacks[static_cast<std::size_t>(i)] <= 0) {
        t.stacks[static_cast<std::size_t>(i)] = buy_in;
        log_event(t, seat_tag(t, i) + " re-buys for " + std::to_string(buy_in));
      }
    }

    const int button = static_cast<int>(hand_no % static_cast<std::uint64_t>(seats));
    const int sb = seats == 2 ? button : (button + 1) % seats;
    const int bb = (sb + 1) % seats;
    if (t.tui) {
      t.log.emplace_back();
      log_event(t, std::string(c_bold()) + "— hand #" + std::to_string(hand_no + 1) + ", button " +
                       c_reset() + seat_name(t, button) + c_bold() + " —" + c_reset());
    } else {
      std::printf("\n%s━━━ HAND #%llu ━━ blinds %lld/%lld ━━━━━━━━━━━━━━━━━━━━%s\n", c_bold(),
                  static_cast<unsigned long long>(hand_no + 1),
                  static_cast<long long>(cfg.small_blind), static_cast<long long>(cfg.big_blind),
                  c_reset());
      for (int i = 0; i < seats; ++i) {
        std::printf("  %s  %6lld", seat_name(t, i, 5).c_str(),
                    static_cast<long long>(t.stacks[static_cast<std::size_t>(i)]));
        if (i == button || i == sb || i == bb) {
          std::printf("  %s%s%s%s%s", c_dim(), i == button ? "btn" : "",
                      i == sb ? (i == button ? " sb" : "sb") : "", i == bb ? "bb" : "", c_reset());
        }
        std::printf("\n");
      }
      std::printf("\n");
    }

    const auto deck = shuffled_deck(rng);
    State s = new_hand(cfg, t.stacks, button, deck);
    ++t.hands_played;
    log_event(t, seat_tag(t, sb) + (sb == 0 ? " post  " : " posts ") +
                     std::to_string(s.street_committed[sb]));
    log_event(t, seat_tag(t, bb) + (bb == 0 ? " post  " : " posts ") +
                     std::to_string(s.street_committed[bb]));

    int shown_board = 0;
    while (!is_terminal(s)) {
      const int seat = s.to_act;
      const LegalActions la = legal_actions(s);
      if (s.board_count > shown_board) {
        if (t.tui) {
          static const char* kStreets[] = {"preflop", "flop", "turn", "river"};
          t.log.emplace_back();
          log_event(t, std::string(c_bold()) + pad_right(kStreets[static_cast<int>(s.street)], 7) +
                           c_reset() + " " +
                           cards_str(s.board + shown_board, s.board_count - shown_board));
        } else {
          show_street(s);
        }
        shown_board = s.board_count;
      }
      if (seat == 0) {
        const auto a = human_action(s, la, t);
        if (!a) {
          farewell(s.stacks[0]);
          return 0;
        }
        announce(s, t, seat, *a);
        apply(s, *a);
      } else {
        const Action a = bot_action(s, la, rng);
        announce(s, t, seat, a);
        apply(s, a);
      }
    }
    // run out any board dealt after the last action (all-in showdowns)
    if (t.tui && s.board_count > shown_board) {
      log_event(t, std::string(c_bold()) + pad_right("board", 7) + c_reset() + " " +
                       cards_str(s.board + shown_board, s.board_count - shown_board));
    }
    show_result(s, t);

    if (t.tui) {
      const bool bust = t.stacks[0] <= 0;
      log_event(
          t, std::string(c_dim()) +
                 (bust ? "you're bust — enter re-buys for " + std::to_string(buy_in) + ", q quits"
                       : "press enter for the next hand") +
                 c_reset());
      draw_frame(s, t, nullptr);
      const auto line = t.editor.read(" > ");
      if (!line || *line == "q" || *line == "quit" || *line == "exit") {
        farewell(t.stacks[0]);
        return 0;
      }
      if (bust) {
        t.stacks[0] = buy_in;
        log_event(t, seat_tag(t, 0) + " re-buys for " + std::to_string(buy_in));
      }
    }
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

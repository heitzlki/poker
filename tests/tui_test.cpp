// Drives `poker play` on a pseudo-terminal and checks the interactive UI,
// which the piped smoke tests never exercise: panel layout, log routing,
// the line editor, ctrl-c, and frame geometry at a chosen terminal size.
// Usage: poker_tui_test <poker-binary>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <csignal>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

int& failures() {
  static int n = 0;
  return n;
}

void check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  failures() += ok ? 0 : 1;
}

void drain(int fd, std::string& out, int ms) {
  const auto end = Clock::now() + std::chrono::milliseconds(ms);
  char buf[4096];
  while (Clock::now() < end) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 100) <= 0) {
      continue;
    }
    const ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) {
      return;
    }
    out.append(buf, static_cast<std::size_t>(n));
  }
}

// Runs one scripted session and returns everything the game printed.
// Sessions must end in enough "q\r" to quit from any prompt.
std::string run_session(const char* bin, const std::vector<const char*>& script,
                        unsigned short rows, unsigned short cols) {
  winsize ws{};
  ws.ws_row = rows;
  ws.ws_col = cols;
  int fd = -1;
  const pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
  if (pid < 0) {
    std::perror("forkpty");
    return {};
  }
  if (pid == 0) {
    ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws); // make sure the size sticks pre-exec
    execl(bin, bin, "play", "--bots", "2", "--seed", "7", static_cast<char*>(nullptr));
    _exit(127);
  }

  std::string out;
  for (const char* cmd : script) {
    drain(fd, out, 400);
    if (write(fd, cmd, std::strlen(cmd)) < 0) {
      break;
    }
  }

  int status = 0;
  bool exited = false;
  const auto deadline = Clock::now() + std::chrono::seconds(20);
  while (Clock::now() < deadline) {
    drain(fd, out, 100);
    if (waitpid(pid, &status, WNOHANG) == pid) {
      exited = true;
      break;
    }
  }
  if (!exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }
  check(exited, "game exits on its own");
  drain(fd, out, 200);
  close(fd);
  return out;
}

std::string strip_ansi(const std::string& in) {
  std::string out;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\033') {
      ++i;
      if (i < in.size() && in[i] == '[') {
        while (i < in.size() && !std::isalpha(static_cast<unsigned char>(in[i]))) {
          ++i;
        }
      }
      continue;
    }
    out += in[i];
  }
  return out;
}

bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// The screen repaints from ESC[H; each repaint is one frame.
std::vector<std::string> frames(const std::string& out) {
  std::vector<std::string> result;
  std::size_t pos = 0;
  while (true) {
    const std::size_t next = out.find("\033[H", pos + 1);
    result.push_back(out.substr(pos, next - pos));
    if (next == std::string::npos) {
      return result;
    }
    pos = next;
  }
}

std::size_t count_in(const std::string& hay, const char* needle) {
  std::size_t n = 0;
  for (std::size_t pos = hay.find(needle); pos != std::string::npos;
       pos = hay.find(needle, pos + 1)) {
    ++n;
  }
  return n;
}

// Visible width in terminal cells; every character we print is one cell.
std::size_t cells(const std::string& line) {
  std::size_t n = 0;
  for (const char c : line) {
    n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
  }
  return n;
}

void gameplay(const char* bin) {
  std::printf("-- gameplay\n");
  // seed 7 deals you 8d 6c on the button: raise preflop, botch a command,
  // take the free continues, fold, leave from whatever prompt comes next
  const auto out = run_session(
      bin, {"r 30\r", "nonsense\r", "\r", "f\r", "\r", "q\r", "q\r", "q\r", "q\r"}, 32, 80);
  const std::string plain = strip_ansi(out);

  check(has(out, "\033[?1049h"), "alternate screen entered");
  check(has(out, "\033[?1049l"), "alternate screen left on quit");
  check(has(plain, "POKER · NLHE 5/10 · 3 seats"), "title bar");
  check(has(plain, "1 you") && has(plain, "2 bot1") && has(plain, "3 bot2"), "all seats listed");
  check(has(plain, "8♦ 6♣"), "your seed-7 hole cards in the panel");
  check(has(plain, "?? ??"), "bot cards hidden");
  check(has(plain, "◀ to act"), "to-act marker");
  check(has(plain, "pot "), "pot shown");
  check(has(plain, "[you]") && has(plain, "[bot1]") && has(plain, "[bot2]"), "log names tagged");
  check(has(plain, "posts"), "blind posts logged");
  check(has(plain, "raise to  30"), "preflop raise echoed with its amount");
  check(has(plain, "unknown command 'nonsense'"), "bad input goes to the log");
  check(has(plain, "flop"), "street dealt and logged");
  check(has(plain, "— hand #"), "hand marker");
  check(has(plain, "you leave with"), "clean farewell");
}

void line_editor(const char* bin) {
  std::printf("-- line editor\n");
  // help once, recall it with the up arrow, then ctrl-u a garbage line away
  // and fold instead ("\x15" is ctrl-u, "\x66" is f, split from "zz")
  const auto out =
      run_session(bin, {"help\r", "\033[A\r", "zz\x15\x66\r", "q\r", "q\r", "q\r", "q\r"}, 32, 80);
  const std::string plain = strip_ansi(out);

  bool recalled = false;
  for (const auto& frame : frames(out)) {
    recalled = recalled || count_in(frame, "commands:") >= 2;
  }
  check(recalled, "up arrow recalls the last command");
  check(!has(plain, "unknown command"), "ctrl-u cleared the garbage before it was sent");
  check(has(plain, "[you]   fold"), "the edited line still folds");
}

void ctrl_c(const char* bin) {
  std::printf("-- ctrl-c\n");
  const auto out = run_session(bin, {"\x03"}, 32, 80);
  check(has(strip_ansi(out), "you leave with"), "ctrl-c leaves the table cleanly");
  check(has(out, "\033[?1049l"), "terminal restored");
}

void geometry(const char* bin) {
  std::printf("-- geometry at 24x60\n");
  const auto out = run_session(bin, {"q\r"}, 24, 60);
  const auto all = frames(out);
  check(all.size() > 1, "at least one frame drawn");

  bool fits = true;
  std::size_t widest = 0;
  for (const auto& frame : all) {
    const std::string plain = strip_ansi(frame);
    std::string line;
    for (const char c : plain + "\n") {
      if (c == '\n' || c == '\r') {
        widest = std::max(widest, cells(line));
        fits = fits && cells(line) <= 60;
        line.clear();
      } else {
        line += c;
      }
    }
  }
  std::printf("     widest line: %zu of 60 cells\n", widest);
  check(fits, "no line overflows the terminal width");
  check(widest == 60, "the rule lines span the full width");
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: poker_tui_test <poker-binary>\n");
    return 2;
  }
  gameplay(argv[1]);
  line_editor(argv[1]);
  ctrl_c(argv[1]);
  geometry(argv[1]);
  return failures() == 0 ? 0 : 1;
}

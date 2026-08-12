#include "ui/glyphs.hpp"

#include <cctype>
#include <cstdlib>
#include <string>

namespace gittop::ui {
namespace {

// Written with designated initializers so the three sets can be read side by
// side and a field added to the struct fails to compile in the two sets that
// forgot it, rather than silently becoming a null pointer that crashes the
// first time somebody selects that mode.

constexpr GlyphSet kAscii{
    .name = "ascii",

    .cursor = ">",
    .rule = "|",
    .bullet = ".",
    .ellipsis = "...",
    .absent = "-",
    .prompt = ">",

    .arrow_up = "^",
    .arrow_down = "v",
    .arrow_left = "<",
    .arrow_right = "->",
    .arrow_enter = "ent",

    // "+" rather than the more obvious "v" for a check, because "v" is already
    // the down arrow and the two appear one row apart in the branch list.
    .check = "+",
    .cross = "x",
    .warning = "!",
    .alert = "!",
    .paused = "=",

    .staged = "*",
    .unstaged = "o",
    .untracked = "o",
    .conflict = "!",

    .node = "*",
    .node_head = "@",
    .lane_vertical = "|",
    .lane_close = "/",
    .lane_open = "\\",

    .branch = "*",
    .detached = "@",
    .ahead = "^",
    .behind = "v",

    .star = "*",
    .fork = "Y",
    .issue = "o",
    .watcher = "@",

    .github = "GH",
    .gitlab = "GL",
    .provider_unknown = "??",

    .ci_success = "+",
    .ci_failed = "x",
    .ci_running = "*",
    .ci_pending = ".",
    .ci_cancelled = "/",
    .ci_skipped = "-",

    .empty_diff = "===",
    .empty_stash = "[ ]",
    .empty_pull = "( )",
    .empty_ci = "...",
    .empty_generic = "- -",

    .axis_tick = "|",
    .axis_origin = "+",
    .track = "-",
    .track_handle = "=",
    .pan_left = "<",
    .pan_right = ">",
    .diff_minus = "-",
    .pause_bars = "||",
    .mask = "***",

    .heat_ramp = {" ", ".", ":", "+", "#"},

    .blocks = {"_", ".", ",", "-", "=", "+", "*", "#"},
    .eighths = {" ", " ", "-", "-", "=", "=", "#", "#"},
    .spinner = {"|", "/", "-", "\\", "|", "/", "-", "\\", "|", "/"},

    .bar_full = "#",
    .bar_track = ".",
};

constexpr GlyphSet kUnicode{
    .name = "unicode",

    .cursor = "▌",
    .rule = "▍",
    .bullet = "·",
    .ellipsis = "…",
    .absent = "—",
    .prompt = "❯",

    .arrow_up = "↑",
    .arrow_down = "↓",
    .arrow_left = "←",
    .arrow_right = "→",
    .arrow_enter = "⏎",

    .check = "✓",
    .cross = "✗",
    .warning = "▲",
    .alert = "⚠",
    .paused = "‖",

    .staged = "●",
    .unstaged = "○",
    .untracked = "○",
    .conflict = "◆",

    .node = "●",
    .node_head = "◉",
    .lane_vertical = "│",
    .lane_close = "╯",
    .lane_open = "╮",

    .branch = "◆",
    .detached = "◇",
    .ahead = "↑",
    .behind = "↓",

    .star = "★",
    .fork = "⑂",
    .issue = "◎",
    .watcher = "◉",

    .github = "⬢",
    .gitlab = "⬡",
    .provider_unknown = "○",

    .ci_success = "✓",
    .ci_failed = "✗",
    .ci_running = "◌",
    .ci_pending = "▷",
    .ci_cancelled = "⊘",
    .ci_skipped = "–",

    .empty_diff = "≡",
    .empty_stash = "⊘",
    .empty_pull = "○",
    .empty_ci = "◌",
    .empty_generic = "◇",

    .axis_tick = "┤",
    .axis_origin = "┼",
    .track = "─",
    .track_handle = "━",
    .pan_left = "◀",
    .pan_right = "▶",
    .diff_minus = "−",
    .pause_bars = "‖",
    .mask = "•••",

    // Shade blocks rather than five tints of one hue: the ramp is then legible
    // as a ramp before any colour is applied to it.
    .heat_ramp = {"·", "░", "▒", "▓", "█"},

    .blocks = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"},
    .eighths = {"▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"},
    .spinner = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"},

    .bar_full = "█",
    .bar_track = "─",
};

// Nerd Font icons live in the private use area, which means two things worth
// stating plainly. A terminal without a patched font draws every one of them as
// a replacement box — hence opt-in only. And some of them are drawn wider than
// the one cell wcwidth reports, which tears a border; that is a property of the
// font rather than of gittop, and the reason the default stays Unicode.
//
// Only the roles with a genuinely better icon are swapped. Everything else falls
// through to the Unicode shape, because an icon that means nothing is worse than
// the geometric one it replaced.
constexpr GlyphSet kNerd{
    .name = "nerd",

    .cursor = "▌",
    .rule = "▍",
    .bullet = "·",
    .ellipsis = "…",
    .absent = "—",
    .prompt = "❯",

    .arrow_up = "↑",
    .arrow_down = "↓",
    .arrow_left = "←",
    .arrow_right = "→",
    .arrow_enter = "⏎",

    .check = "",       // nf-fa-check
    .cross = "",       // nf-fa-times
    .warning = "",     // nf-fa-warning
    .alert = "",       // nf-oct-alert
    .paused = "",      // nf-fa-pause

    .staged = "",      // nf-oct-diff_added
    .unstaged = "",    // nf-oct-diff_modified
    .untracked = "",   // nf-fa-question
    .conflict = "",    // nf-oct-alert

    .node = "",        // nf-oct-git_commit
    .node_head = "◉",
    .lane_vertical = "│",
    .lane_close = "╯",
    .lane_open = "╮",

    .branch = "",      // nf-dev-git_branch
    .detached = "",  // nf-oct-git_commit
    .ahead = "↑",
    .behind = "↓",

    .star = "",        // nf-fa-star
    .fork = "",        // nf-fa-code_fork
    .issue = "",       // nf-oct-issue_opened
    .watcher = "",     // nf-fa-eye

    .github = "",      // nf-fa-github
    .gitlab = "",      // nf-fa-gitlab
    .provider_unknown = "",  // nf-fa-cloud

    .ci_success = "",   // nf-fa-check_circle
    .ci_failed = "",    // nf-fa-times_circle
    .ci_running = "",   // nf-fa-refresh
    .ci_pending = "",   // nf-fa-clock_o
    .ci_cancelled = "", // nf-fa-ban
    .ci_skipped = "",   // nf-fa-minus_circle

    .empty_diff = "",   // nf-oct-diff
    .empty_stash = "",  // nf-fa-archive
    .empty_pull = "",   // nf-oct-git_pull_request
    .empty_ci = "",     // nf-fa-cogs
    .empty_generic = "",// nf-fa-inbox

    .axis_tick = "┤",
    .axis_origin = "┼",
    .track = "─",
    .track_handle = "━",
    .pan_left = "◀",
    .pan_right = "▶",
    .diff_minus = "−",
    .pause_bars = "‖",
    .mask = "•••",

    .heat_ramp = {"·", "░", "▒", "▓", "█"},

    .blocks = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"},
    .eighths = {"▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"},
    .spinner = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"},

    .bar_full = "█",
    .bar_track = "─",
};

bool Contains(const char* haystack, const char* needle) {
  return haystack != nullptr && std::string(haystack).find(needle) != std::string::npos;
}

GlyphMode g_mode = GlyphMode::Unicode;
bool g_resolved = false;

}  // namespace

GlyphMode DetectGlyphMode() {
  const char* term = std::getenv("TERM");

  // The Linux framebuffer console has 512 glyphs and no braille among them, and
  // `dumb` promises nothing at all. Both draw the geometric shapes as blanks,
  // which is worse than an ASCII fallback because a blank looks like a bug.
  if (Contains(term, "dumb") || (term != nullptr && std::string(term) == "linux")) {
    return GlyphMode::Ascii;
  }

  // A non-UTF-8 locale means the terminal is being told to read these bytes as
  // something else, and it will. LC_ALL wins over LC_CTYPE wins over LANG,
  // which is the order the C library itself resolves them in.
  for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
      continue;
    }
    std::string lower(value);
    for (char& c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower.find("utf-8") != std::string::npos || lower.find("utf8") != std::string::npos
               ? GlyphMode::Unicode
               : GlyphMode::Ascii;
  }

  // No locale set at all. POSIX says that is the C locale, which is not UTF-8 —
  // but in practice an unset LANG under a modern terminal emulator is far more
  // often an incomplete environment than a genuine 7-bit one, and the shapes
  // used here are the common ones rather than the exotic ones.
  return GlyphMode::Unicode;
}

GlyphMode GlyphModeNow() {
  if (!g_resolved) {
    g_mode = DetectGlyphMode();
    g_resolved = true;
  }
  return g_mode;
}

void SetGlyphMode(GlyphMode mode) {
  g_mode = mode;
  g_resolved = true;
}

const GlyphSet& glyphs() {
  switch (GlyphModeNow()) {
    case GlyphMode::Ascii:
      return kAscii;
    case GlyphMode::Nerd:
      return kNerd;
    case GlyphMode::Unicode:
      break;
  }
  return kUnicode;
}

std::string GlyphModeName(GlyphMode mode) {
  switch (mode) {
    case GlyphMode::Ascii:
      return "ascii";
    case GlyphMode::Nerd:
      return "nerd";
    case GlyphMode::Unicode:
      break;
  }
  return "unicode";
}

bool ParseGlyphMode(const std::string& text, GlyphMode* out) {
  if (text == "auto") {
    *out = DetectGlyphMode();
    return true;
  }
  if (text == "ascii" || text == "plain" || text == "7bit") {
    *out = GlyphMode::Ascii;
    return true;
  }
  if (text == "unicode") {
    *out = GlyphMode::Unicode;
    return true;
  }
  if (text == "nerd" || text == "nerdfont" || text == "nerd-font") {
    *out = GlyphMode::Nerd;
    return true;
  }
  return false;
}

}  // namespace gittop::ui

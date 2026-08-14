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
    .gone = "x",
    .tag = "#",

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
    .key = "&",

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
    .gone = "✗",
    // Same geometric block as branch and detached rather than a nicer-looking
    // tag character from somewhere else: a font that draws ◆ and ◇ draws this
    // one too, which is not true of the tag glyphs that live off on their own.
    .tag = "◈",

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
    // Not a key or a padlock: U+26BF and the emoji padlocks are missing from
    // enough terminal fonts to draw as tofu, and the emoji ones are double-width
    // and coloured besides. This stays in the geometric family the rest of the
    // set uses, which is the family that actually renders everywhere.
    .key = "◈",

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
// Only the roles with a better icon are swapped. Everything else falls through
// to the Unicode shape, because an icon that means nothing is worse than the
// geometric one it replaced — which is why the arrows and the box shapes here
// are the same characters the Unicode set uses.
//
// The icons are written as `\uXXXX` escapes rather than as the characters
// themselves, and that is not a style preference. Every one of these is a
// private-use code point: it renders as nothing in an editor without a patched
// font, survives no copy-paste that normalises text, and says nothing when it
// goes missing. Twenty-nine of them had gone missing exactly that way and the
// set shipped drawing blanks for a release, because the only way to notice is
// to have a Nerd Font installed and to opt into a set that is opt-in by design.
// An escape is ASCII: it shows up in a diff, in a terminal and in an editor, so
// a lost one is visible before it ships rather than after. The `nf-*` name
// beside each is the Nerd Fonts glyph name and is what to look the code point
// up by — `glyphnames.json` in ryanoasis/nerd-fonts is the table.
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

    .check = "\uf00c",             // nf-fa-check
    .cross = "\uf00d",             // nf-fa-times
    .warning = "\uf071",           // nf-fa-warning
    .alert = "\uf421",             // nf-oct-alert
    .paused = "\uf04c",            // nf-fa-pause

    .staged = "\uf457",            // nf-oct-diff_added
    .unstaged = "\uf459",          // nf-oct-diff_modified
    .untracked = "\uf128",         // nf-fa-question
    .conflict = "\uf421",          // nf-oct-alert

    .node = "\uf417",              // nf-oct-git_commit
    .node_head = "◉",
    .lane_vertical = "│",
    .lane_close = "╯",
    .lane_open = "╮",

    .branch = "\ue725",            // nf-dev-git_branch
    .detached = "\uf417",          // nf-oct-git_commit
    .ahead = "↑",
    .behind = "↓",
    .gone = "\uf127",              // nf-fa-unlink
    .tag = "\uf02b",               // nf-fa-tag

    .star = "\uf005",              // nf-fa-star
    .fork = "\uf126",              // nf-fa-code_fork
    .issue = "\uf41b",             // nf-oct-issue_opened
    .watcher = "\uf06e",           // nf-fa-eye

    .github = "\uf09b",            // nf-fa-github
    .gitlab = "\uf296",            // nf-fa-gitlab
    .provider_unknown = "\uf0c2",  // nf-fa-cloud

    .ci_success = "\uf058",        // nf-fa-check_circle
    .ci_failed = "\uf057",         // nf-fa-times_circle
    .ci_running = "\uf021",        // nf-fa-refresh
    .ci_pending = "\uf017",        // nf-fa-clock_o
    .ci_cancelled = "\uf05e",      // nf-fa-ban
    .ci_skipped = "\uf056",        // nf-fa-minus_circle

    .empty_diff = "\uf440",        // nf-oct-diff
    .empty_stash = "\uf187",       // nf-fa-archive
    .empty_pull = "\uf407",        // nf-oct-git_pull_request
    .empty_ci = "\uf085",          // nf-fa-cogs
    .empty_generic = "\uf01c",     // nf-fa-inbox

    .axis_tick = "┤",
    .axis_origin = "┼",
    .track = "─",
    .track_handle = "━",
    .pan_left = "◀",
    .pan_right = "▶",
    .diff_minus = "−",
    .pause_bars = "‖",
    .mask = "•••",
    .key = "\uf084",               // nf-fa-key

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

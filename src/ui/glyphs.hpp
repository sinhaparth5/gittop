#pragma once

#include <array>
#include <string>

#include "model/remote.hpp"

namespace gittop::ui {

// The same rule the theme layer enforces for colour, applied to characters.
//
// Phase 6 left glyph literals scattered through nine panels — "✓" in four files,
// "○" in five, each one an independent decision about what a terminal can draw.
// That is exactly the trap the colour tokens were built to avoid, and it has the
// same fix: panels name a role, one file decides what the role looks like, and
// swapping the whole set is an edit to that file rather than to every panel.
//
// A raw non-ASCII literal anywhere under src/ui/ outside glyphs.cpp is a bug,
// with two deliberate exceptions: the box-drawing FTXUI itself emits for
// borders, and the ellipsis inside Truncate, which is part of measuring rather
// than part of a panel.
enum class GlyphMode {
  Ascii,    // 7-bit only. A serial console, a non-UTF-8 locale, `TERM=linux`.
  Unicode,  // box drawing, geometric shapes, braille. What any modern terminal has.
  Nerd,     // Unicode plus the Nerd Font private-use icons. Opt-in only; see below.
};

// Every glyph gittop draws, named by what it means rather than by what it is.
//
// Fields are `const char*` and the sets are static, so a panel pays nothing to
// read one — this sits in the same place a colour token does, on the hot path of
// every row of every list.
struct GlyphSet {
  const char* name;

  // ---------------------------------------------------------------- structure
  const char* cursor;    // the bar down the left of the selected row
  const char* rule;      // the accent bar in front of a group heading. Thinner
                         // than the cursor on purpose: two bars of one weight
                         // read as two selections.
  const char* bullet;    // an inert dot: a branch that is not HEAD, a clean file
  const char* ellipsis;  // "… 400 lines above", and the tail of a truncation
  const char* absent;    // a field this provider does not send. Not a zero.
  const char* prompt;    // the caret in front of an input

  // ------------------------------------------------------------------- arrows
  const char* arrow_up;
  const char* arrow_down;
  const char* arrow_left;
  const char* arrow_right;
  const char* arrow_enter;

  // ----------------------------------------------------------------- verdicts
  const char* check;
  const char* cross;
  const char* warning;  // the triangle on the operation banner
  const char* alert;    // the sign on a destructive confirm
  const char* paused;   // a poll that has stopped itself

  // -------------------------------------------------------- working tree state
  // Paired with a letter at every call site, so these carry emphasis rather than
  // meaning: the row still reads with no colour and no glyph support at all.
  const char* staged;
  const char* unstaged;
  const char* untracked;
  const char* conflict;

  // ------------------------------------------------------------- commit graph
  const char* node;
  const char* node_head;
  const char* lane_vertical;
  const char* lane_close;
  const char* lane_open;

  // ----------------------------------------------------------------- branches
  const char* branch;
  const char* detached;  // HEAD on a commit rather than on a branch
  const char* ahead;
  const char* behind;
  const char* gone;  // an upstream that is configured but no longer there
  const char* tag;   // a tag, beside `branch` in a list that holds both

  // ---------------------------------------------------------- remote counters
  const char* star;
  const char* fork;
  const char* issue;
  const char* watcher;

  // ---------------------------------------------------------------- providers
  // The mark that goes in front of a provider's name, a host, or a remote. Read
  // through ProviderGlyph() rather than by hand: five panels each had their own
  // copy of that switch, which is five places for a fourth provider to be
  // forgotten.
  const char* github;
  const char* gitlab;
  const char* provider_unknown;

  // ----------------------------------------------------------------------- CI
  // Separate from the general verdicts because pending and skipped have no
  // general equivalent, and because a set may want a differently-weighted mark
  // for a run that is still going than for a check that passed.
  const char* ci_success;
  const char* ci_failed;
  const char* ci_running;
  const char* ci_pending;
  const char* ci_cancelled;
  const char* ci_skipped;

  // ------------------------------------------------------------ empty states
  // Centred and large, so these are the one place a multi-cell ASCII fallback
  // costs nothing: there is no column for it to push out of line.
  const char* empty_diff;
  const char* empty_stash;
  const char* empty_pull;
  const char* empty_ci;
  const char* empty_generic;

  // ------------------------------------------------------------ drawing parts
  const char* axis_tick;
  const char* axis_origin;
  const char* track;
  const char* track_handle;
  const char* pan_left;
  const char* pan_right;
  const char* diff_minus;  // the − in "−12". A hyphen there reads as a flag.
  const char* pause_bars;
  const char* mask;  // what replaces the userinfo in a URL that carried a token
  const char* key;   // the sign-in pane, and an authenticated remote

  // Ascending visual weight. The heatmap ramps through this as well as through
  // colour, which is what makes it readable under NO_COLOR — five greens are
  // five identical squares to a terminal that will not draw them.
  std::array<const char*, 5> heat_ramp;

  std::array<const char*, 8> blocks;   // sparkline columns, ▁ through █
  std::array<const char*, 8> eighths;  // sub-cell bar fill, ▏ through █
  std::array<const char*, 10> spinner;

  const char* bar_full;
  const char* bar_track;
};

const GlyphSet& glyphs();

// The mark for one provider, from whichever set is active. Everything that
// prints a provider's name, its host, or a remote pointing at it puts this in
// front, so the same repository is recognisable at a glance from the remote
// panel, the CI header, the pull list, the sign-in pane and the settings page.
std::string ProviderGlyph(model::Provider provider);

// The GitHub and GitLab marks are the one place the nerd icons are worth having
// on their own terms, and the reason is that they are *logos*: no arrangement of
// geometric shapes is the octocat or the tanuki, so unicode here is an
// approximation in a way that `✓` for a passing check is not. Hence a switch of
// their own — on it, the two provider marks come from the nerd set whatever
// `theme.icons` says, and nothing else does.
//
// Off by default, for the same reason auto never picks the whole nerd set: a
// font without the private-use icons draws tofu, and gittop cannot ask. It is
// ignored in ascii mode, where the terminal has already said it wants 7 bits.
bool ProviderLogos();
void SetProviderLogos(bool on);

// Auto never picks Nerd. There is no way to ask a terminal whether the font it
// is using has the private-use icons in it, and guessing wrong fills the screen
// with tofu — so the nerd set is something a user turns on, never something
// gittop turns on for them. Ascii is chosen when the locale is not UTF-8 or the
// terminal is one known not to have the shapes (`dumb`, the Linux console),
// Unicode otherwise.
GlyphMode DetectGlyphMode();
GlyphMode GlyphModeNow();
void SetGlyphMode(GlyphMode mode);
std::string GlyphModeName(GlyphMode mode);

// "auto", "ascii", "unicode", "nerd". False on anything else, so a mistyped
// config line is reported by name rather than silently ignored.
bool ParseGlyphMode(const std::string& text, GlyphMode* out);

}  // namespace gittop::ui

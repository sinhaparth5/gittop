#pragma once

#include <cstddef>
#include <cstdint>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

#include "ui/theme.hpp"

namespace gittop::ui {

// "3d", "2mo", "now" — an age in at most four cells, so a column of them stays
// aligned. `now` is the current epoch time, passed in rather than read here so
// every row of one render agrees about when it is.
std::string RelativeTime(std::int64_t when, std::int64_t now);

// ---------------------------------------------------------------- measurement

// Cells, not bytes and not code points. A CJK ideograph is one code point and
// three bytes and *two cells*, and it is the cell count a border has to agree
// with — get this wrong and the right-hand edge of a panel tears the moment
// somebody's commit message is in Japanese.
int TextWidth(const std::string& text);

// At most `cells` wide, with an ellipsis where it cut. Never splits a glyph, and
// never leaves the first half of a double-width one behind — that half renders
// as a broken box and takes the border with it.
std::string Truncate(const std::string& text, int cells);

// Truncated *and* padded to exactly `cells`. What a column wants: a short value
// and a long one then occupy the same width, so nothing to the right of them
// jitters as the list scrolls or a refresh lands.
std::string Fit(const std::string& text, int cells);

// Right-aligned in `cells`, for a number. Digits are the thing that must not
// move: a count that grows from 9 to 10 shifts every column after it unless the
// field is fixed and the padding goes on the left.
std::string Rjust(const std::string& text, int cells);

// ---------------------------------------------------------------- panel frame

// The border a panel is drawn with. One setting for the whole program rather
// than per panel: a screen with three border weights on it looks like three
// programs. `Rounded` is the default because it reads as one continuous stroke
// at the corners where `Light` shows a visible join on most fonts.
enum class PanelBorder { Rounded, Light, Heavy, Double };

bool ParsePanelBorder(const std::string& text, PanelBorder* out);
std::string PanelBorderName(PanelBorder border);
PanelBorder PanelBorderNow();
void SetPanelBorder(PanelBorder border);

struct PanelStyle {
  // The panel the keys are going to. Exactly one on screen at a time, and it
  // gets the accent border and the bright title; everything else recedes. A
  // dashboard where every panel is equally loud is a dashboard where the cursor
  // is impossible to find.
  bool focused = true;

  // The panel is reporting a failure rather than showing data. Border and title
  // both go to the danger colour, which is the one case where a panel outranks
  // the focus rule — an error nobody looked at is still an error.
  bool alarm = false;

  // Right-hand side of the title bar: a count, a provider, a state. Dim, so it
  // annotates the title instead of competing with it.
  // Explicitly initialized, empty though it is: without it GCC counts this
  // member as uninitialized in every designated-initializer call site and
  // -Wmissing-field-initializers fires on all seventeen of them.
  std::string note = {};
};

// Every framed panel in the program goes through here. That is what makes the
// border style one config line rather than seventeen, and what makes "which
// panel has the cursor" a question with a visible answer.
ftxui::Element Panel(const std::string& title, ftxui::Element content, PanelStyle style = {});

// The configured border as a bare decorator, for the three frames that are not
// panels: the summary card row and the overlays, which carry their own titles.
ftxui::Decorator FramedBorder();

// ------------------------------------------------------------- spacing scale

// One, two, four. Everything on screen sits on one of these — one cell inside a
// group, two between groups, four to indent a block under a heading. Three
// different paddings that each looked about right at the time is what makes a
// layout read as unplanned even when nothing in it is wrong.
inline constexpr int kSpaceTight = 1;
inline constexpr int kSpace = 2;
inline constexpr int kSpaceWide = 4;

ftxui::Element Gap(int cells = kSpace);

// A list that scrolls, with the indicator down its right edge — except in ASCII
// mode, where FTXUI draws that indicator with heavy box characters that are the
// one part of its line-drawing set a 7-bit or CP437 terminal does not have. A
// torn column there is worse than no indicator.
ftxui::Element Scrollable(ftxui::Element list);

// ----------------------------------------------------------------- indicators

// A bar whose fill is shaded along a ramp and drawn with eighth-blocks, so a
// 40% value looks 40% full even in a twelve-cell box. FTXUI's stock gauge is a
// single flat color and quantises to whole cells, which is what made the old
// summary row look blocky.
ftxui::Element GradientBar(float ratio, Ramp ramp, Swatch trough);

// One column per value, height by ratio to `peak`, colour ramped with height.
// Zero draws a floor dot rather than a blank, so a gap in the data is visibly a
// zero and not a column that failed to render.
ftxui::Element Sparkline(const int* values, std::size_t count, int peak, Ramp ramp);
ftxui::Element Sparkline(const std::vector<int>& values, int peak, Ramp ramp);

// Placeholder rows for a panel whose data is still in flight. A blank panel and
// a panel that finished with nothing in it look identical, and the difference
// is the whole question the user is asking while they wait. `frame` slides the
// highlight along, so it reads as working rather than as stuck.
ftxui::Element SkeletonRows(int rows, int frame);

// Key hint drawn as a raised chip. Keeping this in one place is what stops the
// footer and the overlays from drifting apart.
ftxui::Element Chip(const std::string& key, const std::string& label);

// Path with the directory dimmed and the filename bright, so a long list scans
// by filename instead of by prefix. `cells` caps the whole thing, cutting the
// directory first — the filename is the part worth keeping.
ftxui::Element PathText(const std::string& path, bool emphasised, int cells = 0);

// A remote URL with its userinfo replaced, for anywhere one reaches the screen.
// A remote configured as https://user:token@host/... has the token in its URL,
// and there is exactly one rule about those: never render one. Shared rather
// than private to the remote panel because the push confirmation prints a URL
// too, and the second caller is where a rule like this quietly stops holding.
std::string SafeUrl(const std::string& url);

// ---------------------------------------------------------------------- motion

// Every animation in the program asks this first: the eased bars, the spinner,
// the toast fade, the skeleton shimmer and the splash. Off means each of them
// snaps to its final state rather than being removed, so nothing disappears —
// a reduced-motion setting that also hides information is a worse setting.
bool ReducedMotion();
void SetReducedMotion(bool reduced);

// How far up the overlay on top is: 0 the instant it opens, 1 once it has
// arrived. A terminal has no compositor, so a popup cannot slide or scale — but
// it can be drawn emerging out of the background, and that is one number read in
// one place (PaneFrame) rather than a parameter threaded through seven panes.
float OverlayReveal();
void SetOverlayReveal(float reveal);

// A colour mixed toward the background by the current reveal. Identity once the
// overlay is up, which is nearly always, so this costs nothing on a steady frame.
Swatch Emerging(Swatch swatch);

// Repeats between 0 and 1 on wall time, for anything that has to look alive
// rather than progressing: the tint behind a CI run that is still going. Pinned
// at its brightest under reduced motion, so a running row stays distinguishable
// from a finished one.
float Pulse(int frame);

// One frame of the spinner. `frame` is App's counter, which advances on wall
// time rather than per rendered frame so the spin rate does not follow the
// frame rate. Negative values are handled, because the counter is a plain int
// and will eventually wrap. Under reduced motion this is a fixed glyph.
std::string SpinnerFrame(int frame);

}  // namespace gittop::ui

#include "ui/history_panel.hpp"

#include <algorithm>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "git/graph.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using model::GraphCell;

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

// Past this the gutter costs more room than the branching tells you. Deeper
// lanes still get their commits listed, just without a column of their own.
constexpr int kMaxDrawnLanes = 8;

// Fixed columns. A column whose width follows its longest current value is a
// column that shifts every time the list scrolls, and a number that moves while
// you are reading it is worse than a number that is cut short.
constexpr int kAuthorCells = 16;
constexpr int kBranchNameCells = 24;
constexpr int kTrackingCells = 12;

// A heatmap cell's colour *and* its shape, from one place.
//
// Five tints of one hue is five identical squares to a terminal in sixteen-colour
// mode and to a reader with deuteranopia; the shade ramp carries the same reading
// without any colour at all. Floored at a quarter of the way up the colour ramp
// so a single-commit day is visibly a day something happened rather than one that
// fades into the empty cells.
std::pair<Rgb, const char*> HeatCell(int count, int peak) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();
  if (count <= 0) {
    return {t.surface_alt.rgb, g.heat_ramp[0]};
  }
  const float ratio =
      peak > 0 ? std::min(1.0F, static_cast<float>(count) / static_cast<float>(peak)) : 1.0F;
  const auto step = static_cast<std::size_t>(
      std::clamp(1 + static_cast<int>(ratio * 3.999F), 1, 4));
  return {Mix(t.staged_ramp.from, t.staged_ramp.to, 0.25F + (0.75F * ratio)), g.heat_ramp[step]};
}

std::string CellGlyph(GraphCell cell, bool head) {
  const GlyphSet& g = glyphs();
  switch (cell) {
    case GraphCell::Node:
      return head ? g.node_head : g.node;
    case GraphCell::Through:
      return g.lane_vertical;
    case GraphCell::Merge:
      return g.lane_close;
    case GraphCell::Branch:
      return g.lane_open;
    case GraphCell::Empty:
      break;
  }
  return " ";
}

Element Gutter(const model::Commit& commit, int width) {
  const Theme& t = theme();
  Elements cells;
  cells.reserve(static_cast<std::size_t>(width));

  for (int lane = 0; lane < width; ++lane) {
    const GraphCell cell = lane < static_cast<int>(commit.row.size())
                               ? commit.row[static_cast<std::size_t>(lane)]
                               : GraphCell::Empty;
    const Swatch lane_color = t.graph[static_cast<std::size_t>(lane) % t.graph.size()];
    Element glyph = text(CellGlyph(cell, commit.is_head)) | color(lane_color);
    if (cell == GraphCell::Node) {
      glyph = glyph | bold;
    }
    cells.push_back(std::move(glyph));
  }
  return hbox(std::move(cells));
}

// The cells RefBadges is about to take. Computed from the same rule it renders
// by rather than estimated, because the summary's budget is what is left after
// it and an estimate that is one cell out puts an ellipsis where there was room.
int RefBadgeWidth(const model::Commit& commit) {
  const std::size_t shown = std::min<std::size_t>(commit.refs.size(), 2);
  int cells = 0;
  for (std::size_t i = 0; i < shown; ++i) {
    cells += TextWidth(commit.refs[i]) + 3;  // a space either side, then the gap
  }
  if (commit.refs.size() > shown) {
    cells += TextWidth("+" + std::to_string(commit.refs.size() - shown)) + 1;
  }
  return cells;
}

Element RefBadges(const model::Commit& commit) {
  const Theme& t = theme();
  Elements badges;

  // Two is enough to say where you are. A commit that half a dozen refs point
  // at would otherwise push the summary off the row entirely.
  const std::size_t shown = std::min<std::size_t>(commit.refs.size(), 2);
  for (std::size_t i = 0; i < shown; ++i) {
    const Swatch fill = commit.is_head && i == 0 ? t.accent : t.untracked;
    badges.push_back(text(" " + commit.refs[i] + " ") | bold | color(t.bg) | bgcolor(fill));
    badges.push_back(text(" "));
  }
  if (commit.refs.size() > shown) {
    badges.push_back(text("+" + std::to_string(commit.refs.size() - shown) + " ") |
                     color(t.text_faint));
  }
  return badges.empty() ? text("") : hbox(std::move(badges));
}

}  // namespace

Element CommitList(const model::HistorySnapshot& history, int selected, int width,
                   std::vector<Box>* row_boxes) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();

  if (history.empty()) {
    if (row_boxes != nullptr) {
      row_boxes->clear();
    }
    return vbox({
        filler(),
        hbox({filler(), text("no commits yet") | color(t.text), filler()}),
        hbox({filler(), text("make one from the status view with c") | color(t.text_faint),
              filler()}),
        filler(),
    });
  }

  const int lanes = std::min(git::LaneWidth(history.commits), kMaxDrawnLanes);
  const auto now = static_cast<std::int64_t>(std::time(nullptr));

  if (row_boxes != nullptr) {
    row_boxes->assign(history.commits.size(), Box());
  }

  Elements rows;
  rows.reserve(history.commits.size() + 1);

  for (std::size_t i = 0; i < history.commits.size(); ++i) {
    const model::Commit& commit = history.commits[i];
    const bool is_selected = static_cast<int>(i) == selected;

    // Cursor, space, gutter, gap, short id, gap  |  gap, author, gap, age, space
    // — plus the panel's own two border columns. Everything on the row except
    // the summary and the badges is a fixed width, which is what makes this
    // arithmetic rather than a guess.
    const int chrome = 2 + 1 + 1 + lanes + 2 + 7 + 2 + 2 + kAuthorCells + 2 + 4 + 1;
    const int summary_cells = std::max(8, width - chrome - RefBadgeWidth(commit));

    Element row = hbox({
        text(is_selected ? g.cursor : " ") | color(t.accent),
        text(" "),
        Gutter(commit, lanes),
        text("  "),
        text(commit.short_id) | color(t.accent),
        text("  "),
        RefBadges(commit),
        text(Truncate(commit.summary, summary_cells)) |
            color(is_selected ? t.text : t.text_dim),
        filler(),
        text("  "),
        // Fixed width, so the author column has a left edge instead of one that
        // moves with whoever happens to be on the row above.
        text(Fit(commit.author, kAuthorCells)) | color(t.text_faint),
        text("  "),
        text(Rjust(RelativeTime(commit.time, now), 4)) | color(t.text_faint),
        text(" "),
    });

    if (is_selected) {
      row = row | bgcolor(t.surface_alt) | focus;
    }
    if (row_boxes != nullptr) {
      row = std::move(row) | reflect((*row_boxes)[i]);
    }
    rows.push_back(std::move(row));
  }

  if (history.truncated) {
    rows.push_back(hbox({
        text("   "),
        text("history truncated at " + std::to_string(history.walked) + " commits") |
            color(t.text_faint),
    }));
  }

  return Scrollable(vbox(std::move(rows)));
}

Element ActivityPanel(const model::HistorySnapshot& history) {
  const Theme& t = theme();
  const auto days = static_cast<int>(history.activity.size());
  if (days == 0) {
    return text("  no activity in the window") | color(t.text_faint);
  }

  // Epoch day 0 was a Thursday, so shifting by four lands Sunday on zero.
  const int first_weekday =
      static_cast<int>((((history.activity_start_day + 4) % 7) + 7) % 7);
  const int columns = (days + first_weekday + 6) / 7;

  const auto weekday_label = [](int weekday) -> std::string {
    switch (weekday) {
      case 1:
        return "Mon";
      case 3:
        return "Wed";
      case 5:
        return "Fri";
      default:
        return "   ";
    }
  };

  Elements grid_rows;
  for (int weekday = 0; weekday < 7; ++weekday) {
    Elements cells;
    cells.push_back(text(" " + weekday_label(weekday) + " ") | color(t.text_faint));

    for (int column = 0; column < columns; ++column) {
      const int bucket = (column * 7) + weekday - first_weekday;
      if (bucket < 0 || bucket >= days) {
        cells.push_back(text("  "));
        continue;
      }
      const int count = history.activity[static_cast<std::size_t>(bucket)];
      const auto [fill, glyph] = HeatCell(count, history.activity_max);
      cells.push_back(text(std::string(glyph) + " ") | color(ToColor(fill)));
    }
    grid_rows.push_back(hbox(std::move(cells)));
  }

  Elements legend;
  legend.push_back(text("    less ") | color(t.text_faint));
  // Drawn from the same function as the grid rather than from a parallel copy
  // of the formula, which is how a legend ends up describing a ramp the cells
  // above it no longer use.
  for (int step = 0; step < 5; ++step) {
    const auto [fill, glyph] = HeatCell(step, 4);
    legend.push_back(text(std::string(glyph) + " ") | color(ToColor(fill)));
  }
  legend.push_back(text("more") | color(t.text_faint));
  legend.push_back(filler());

  std::size_t total = 0;
  for (const int count : history.activity) {
    total += static_cast<std::size_t>(count);
  }
  legend.push_back(text(std::to_string(total) + " commits in " + std::to_string(days) +
                        " days  ") |
                   color(t.text_faint));

  grid_rows.push_back(hbox(std::move(legend)));
  return vbox(std::move(grid_rows));
}

Element BranchList(const model::HistorySnapshot& history, int selected, int width,
                   std::vector<Box>* row_boxes) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();

  if (history.branches.empty()) {
    if (row_boxes != nullptr) {
      row_boxes->clear();
    }
    return vbox({
        filler(),
        hbox({filler(), text("no local branches") | color(t.text_faint), filler()}),
        filler(),
    });
  }

  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  if (row_boxes != nullptr) {
    row_boxes->assign(history.branches.size(), Box());
  }
  Elements rows;

  for (std::size_t i = 0; i < history.branches.size(); ++i) {
    const model::Branch& branch = history.branches[i];
    const bool is_selected = static_cast<int>(i) == selected;

    // Both counts padded to the same field whether or not they are present, so
    // the upstream name after them starts in the same column on every row.
    Elements tracking;
    if (branch.upstream_gone) {
      // Deliberately not ahead/behind: there is nothing left to be ahead of, and
      // a count against a ref that describes a branch nobody can push to any
      // more reads as a fact about the remote when it is a fact about a stale
      // local ref. The glyph carries it as well as the colour, which is the same
      // rule every status in gittop follows.
      tracking.push_back(text(Fit(g.gone + std::string(" gone"), kTrackingCells)) |
                         color(t.warning));
    } else if (!branch.has_upstream) {
      tracking.push_back(text(Fit("no upstream", kTrackingCells)) | color(t.text_faint));
    } else if (branch.ahead == 0 && branch.behind == 0) {
      tracking.push_back(text(Fit("in sync", kTrackingCells)) | color(t.text_faint));
    } else {
      tracking.push_back(text(Rjust(branch.ahead > 0 ? g.ahead + std::to_string(branch.ahead)
                                                     : std::string(),
                                    5)) |
                         color(t.staged));
      tracking.push_back(text(" "));
      tracking.push_back(text(Fit(branch.behind > 0 ? g.behind + std::to_string(branch.behind)
                                                    : std::string(),
                                  kTrackingCells - 6)) |
                         color(t.unstaged));
    }

    Elements parts{
        text(is_selected ? g.cursor : " ") | color(t.accent),
        text(" "),
        text(branch.is_head ? g.branch : g.bullet) | bold |
            color(branch.is_head ? t.staged : t.text_faint),
        text("  "),
        text(Fit(branch.name, kBranchNameCells)) | bold |
            color(is_selected || branch.is_head ? t.text : t.text_dim),
        text("  "),
        hbox(std::move(tracking)),
        text(Fit(branch.upstream, 24)) | color(t.text_faint),
        filler(),
    };

    // The sparkline is the first thing to go as the terminal narrows: it is the
    // only column on the row that is a nicety rather than a fact you came for.
    if (width >= 96) {
      parts.push_back(text("  "));
      if (branch.velocity_known && branch.velocity_max > 0) {
        parts.push_back(Sparkline(branch.velocity.data(), branch.velocity.size(),
                                  branch.velocity_max, t.untracked_ramp));
      } else {
        // Twelve quiet weeks, drawn as twelve floor dots — the same mark the
        // sparkline itself uses for a zero. A blank gap here would be
        // indistinguishable from a column that failed to render, and a branch
        // nobody has touched this quarter is a thing worth being able to see.
        std::string floor;
        for (std::size_t week = 0; week < model::kVelocityWeeks; ++week) {
          floor += g.bullet;
        }
        parts.push_back(text(floor) | color(t.surface_alt));
      }
      parts.push_back(text("  "));
    }

    parts.push_back(text(Rjust(branch.time > 0 ? RelativeTime(branch.time, now) : "", 4)) |
                    color(t.text_faint));
    parts.push_back(text("  "));

    Element row = hbox(std::move(parts));

    if (is_selected) {
      row = row | bgcolor(t.surface_alt) | focus;
    }
    if (row_boxes != nullptr) {
      row = std::move(row) | reflect((*row_boxes)[i]);
    }
    rows.push_back(std::move(row));
  }

  return Scrollable(vbox(std::move(rows)));
}

}  // namespace gittop::ui

#include "ui/remote_panel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using model::FetchState;
using model::Provider;
using model::RemoteSnapshot;
using model::TokenSource;

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

std::int64_t Now() {
  return static_cast<std::int64_t>(std::time(nullptr));
}

// Thin separators every three digits. A star count is a number people compare
// at a glance, and 12483 reads slower than 12 483 does.
std::string Grouped(int value) {
  if (value < 0) {
    return glyphs().absent;
  }
  const std::string digits = std::to_string(value);
  std::string out;
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) {
      out.push_back(' ');
    }
    out.push_back(digits[i]);
  }
  return out;
}

std::string Ago(std::int64_t when) {
  if (when <= 0) {
    return "unknown";
  }
  const std::int64_t delta = std::max<std::int64_t>(0, Now() - when);
  if (delta < 60) {
    return "just now";
  }
  const auto plural = [](std::int64_t n, const char* unit) {
    return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
  };
  if (delta < 3600) {
    return plural(delta / 60, "minute");
  }
  if (delta < 86400) {
    return plural(delta / 3600, "hour");
  }
  if (delta < 86400 * 30) {
    return plural(delta / 86400, "day");
  }
  if (delta < 86400 * 365) {
    return plural(delta / (86400 * 30), "month");
  }
  return plural(delta / (86400 * 365), "year");
}

std::string Until(std::int64_t when) {
  if (when <= 0) {
    return {};
  }
  const std::int64_t delta = when - Now();
  if (delta <= 0) {
    return "resets now";
  }
  if (delta < 60) {
    return "resets in " + std::to_string(delta) + "s";
  }
  if (delta < 3600) {
    return "resets in " + std::to_string(delta / 60) + " min";
  }
  return "resets in " + std::to_string(delta / 3600) + "h";
}

Element Section(const std::string& title, Element body, bool focused = false) {
  return Panel(title, std::move(body), {.focused = focused});
}

// Label above, number below. Same shape as the status cards, so the two views
// feel like one product rather than two screens that happen to share a border.
Element Tile(const std::string& glyph, const std::string& label, const std::string& value,
             Swatch accent, bool known) {
  const Theme& t = theme();
  constexpr int kLabelWidth = 11;
  return vbox({
             hbox({
                 text(glyph) | color(known ? accent : t.text_faint),
                 text(" " + label) | color(t.text_faint) | size(WIDTH, EQUAL, kLabelWidth),
                 filler(),
             }),
             hbox({
                 text(value) | bold | color(known ? t.text : t.text_faint),
                 filler(),
             }),
         }) |
         xflex;
}

Element DetailRow(const std::string& label, Element value) {
  const Theme& t = theme();
  return hbox({
      text("  "),
      text(label) | color(t.text_faint) | size(WIDTH, EQUAL, 16),
      std::move(value) | xflex,
  });
}

Element DetailText(const std::string& label, const std::string& value, bool dim = false) {
  const Theme& t = theme();
  return DetailRow(label, text(value.empty() ? glyphs().absent : value) |
                              color(value.empty() || dim ? t.text_faint : t.text));
}

// Identity block. Drawn from the parsed remote alone, so it is on screen while
// the request is still in flight rather than appearing when the reply lands.
Element Identity(const RemoteSnapshot& snapshot) {
  const Theme& t = theme();
  const auto& ref = snapshot.ref;

  // Just the state here. Which variable or which file it came from is a whole
  // config path long, which is enough to push the hostname off its own header;
  // it belongs in the details list, where there is room for it.
  Elements auth;
  if (snapshot.authenticated()) {
    auth.push_back(text(glyphs().check) | color(t.success));
    auth.push_back(text(" authenticated") | color(t.text_dim));
  } else {
    auth.push_back(text(glyphs().provider_unknown) | color(t.text_faint));
    auth.push_back(text(" anonymous") | color(t.text_faint));
  }

  Elements header{
      text(" " + ProviderGlyph(ref.provider) + " ") | bold | color(t.accent),
      text(model::ProviderName(ref.provider)) | bold | color(t.text),
      text("  " + ref.host) | color(t.text_dim),
  };
  if (ref.self_hosted) {
    header.push_back(text("  self-hosted ") | color(t.bg) | bgcolor(t.text_faint));
  }
  header.push_back(filler());
  for (Element& element : auth) {
    header.push_back(std::move(element));
  }
  header.push_back(text(" "));

  Elements name{
      text("  "),
      text(ref.full_name().empty() ? "unknown repository" : ref.full_name()) | bold |
          color(t.accent),
  };
  if (!snapshot.info.visibility.empty()) {
    const bool open = snapshot.info.visibility == "public";
    name.push_back(text("   " + snapshot.info.visibility + " ") |
                   color(open ? t.text_faint : t.warning));
  }
  if (snapshot.info.archived) {
    name.push_back(text(" archived ") | bold | color(t.bg) | bgcolor(t.warning));
  }
  name.push_back(filler());

  Elements body{hbox(std::move(header)), separator() | color(t.border), hbox(std::move(name))};

  if (!snapshot.info.description.empty()) {
    body.push_back(hbox({
        text("  "),
        paragraph(snapshot.info.description) | color(t.text_dim) | xflex,
    }));
  }
  body.push_back(text(""));

  return vbox(std::move(body)) | FramedBorder() | color(t.border) | bgcolor(t.surface);
}

Element Tiles(const RemoteSnapshot& snapshot) {
  const Theme& t = theme();
  const auto& info = snapshot.info;
  const auto gap = [] { return text("  "); };
  const auto rule = [&t] { return separator() | color(t.border); };

  std::vector<Element> tiles;
  const GlyphSet& g = glyphs();
  tiles.push_back(Tile(g.star, "STARS", Grouped(info.stars), t.warning, info.stars >= 0));
  tiles.push_back(Tile(g.fork, "FORKS", Grouped(info.forks), t.untracked, info.forks >= 0));
  tiles.push_back(
      Tile(g.issue, "ISSUES", Grouped(info.open_issues), t.unstaged, info.open_issues >= 0));
  // GitLab has no watcher count. Showing an em dash beats showing a zero that
  // would read as a fact about the repository rather than about the API.
  tiles.push_back(Tile(g.watcher, "WATCHERS", Grouped(info.watchers), t.staged,
                       info.watchers >= 0));

  Elements row{gap()};
  for (std::size_t i = 0; i < tiles.size(); ++i) {
    row.push_back(std::move(tiles[i]));
    row.push_back(gap());
    if (i + 1 < tiles.size()) {
      row.push_back(rule());
      row.push_back(gap());
    }
  }

  return hbox(std::move(row)) | FramedBorder() | color(t.border) | bgcolor(t.surface);
}

Element Budget(const RemoteSnapshot& snapshot) {
  const Theme& t = theme();
  const auto& rate = snapshot.rate;

  if (!rate.known) {
    return Section("API BUDGET", vbox({
                                     hbox({
                                         text("  "),
                                         text("this host does not report a rate limit") |
                                             color(t.text_faint),
                                         filler(),
                                     }),
                                     text(""),
                                 }));
  }

  const float ratio =
      rate.limit > 0 ? std::clamp(static_cast<float>(rate.remaining) / static_cast<float>(rate.limit),
                                  0.0F, 1.0F)
                     : 0.0F;

  // The bar shows what is left, so its colour has to travel the other way from
  // a usage bar: green while there is room, red when there is not.
  Ramp ramp = t.staged_ramp;
  Swatch accent = t.success;
  std::string state = "healthy";
  if (ratio < 0.2F) {
    ramp = t.conflict_ramp;
    accent = t.danger;
    state = "nearly exhausted";
  } else if (ratio < 0.5F) {
    ramp = t.unstaged_ramp;
    accent = t.warning;
    state = "over half spent";
  }

  const std::string reset = Until(rate.reset);

  return Section("API BUDGET", vbox({
                                   hbox({
                                       text("  "),
                                       text(Grouped(rate.remaining)) | bold | color(accent),
                                       text(" / " + Grouped(rate.limit) + " requests") |
                                           color(t.text_faint),
                                       text("   " + state) | color(t.text_dim),
                                       filler(),
                                       text(reset.empty() ? "" : reset + "  ") |
                                           color(t.text_faint),
                                   }),
                                   text(""),
                                   hbox({text(" "), GradientBar(ratio, ramp, t.surface_alt) | xflex,
                                         text(" ")}),
                               }));
}

Element Details(const RemoteSnapshot& snapshot, bool include_counts,
                const std::string& transports) {
  const Theme& t = theme();
  const auto& ref = snapshot.ref;

  Elements rows;

  // On a narrow terminal the tile row is dropped, so the numbers it carried
  // move here rather than disappearing.
  if (include_counts) {
    const auto& info = snapshot.info;
    const GlyphSet& g = glyphs();
    rows.push_back(DetailRow(
        "activity", hbox({
                        text(std::string(g.star) + " " + Grouped(info.stars)) | color(t.warning),
                        text(std::string("   ") + g.fork + " " + Grouped(info.forks)) |
                            color(t.untracked),
                        text(std::string("   ") + g.issue + " " + Grouped(info.open_issues)) |
                            color(t.unstaged),
                        filler(),
                    })));
  }

  // Names the variable or the file, never the value. There is deliberately no
  // way to make gittop print a token, masked or otherwise.
  std::string auth;
  switch (snapshot.token_source) {
    case TokenSource::Environment:
      auth = snapshot.token_origin + "  (environment)";
      break;
    case TokenSource::ConfigFile:
      auth = snapshot.token_origin + "  (config file)";
      break;
    case TokenSource::SignedIn:
      // Says outright that it is not on disk. Everything else here names a
      // place the token can be found again; this one names the fact that it
      // cannot, which is the only thing about it worth knowing.
      auth = "signed in  (this session only)";
      break;
    case TokenSource::None:
      auth = std::string("none ") + glyphs().absent + " reading anonymously";
      break;
  }

  Elements tail{
      DetailRow("token", text(auth) | color(snapshot.authenticated() ? t.text : t.text_faint)),
      DetailText("default branch", snapshot.info.default_branch),
      DetailText("last push", Ago(snapshot.info.last_activity)),
      DetailRow("remote", hbox({
                              text(ref.name) | color(t.text),
                              text("  " + SafeUrl(ref.url)) | color(t.text_faint),
                          })),
      DetailText("web", snapshot.info.web_url.empty() ? ref.web_url : snapshot.info.web_url, true),
      DetailText("api", ref.api_base, true),
      DetailText("transports", transports),
  };
  for (Element& row : tail) {
    rows.push_back(std::move(row));
  }
  if (snapshot.fetched_at > 0) {
    rows.push_back(DetailText("fetched", Ago(snapshot.fetched_at)));
  }
  rows.push_back(text(""));

  // Scrollable rather than silently truncated: this is the panel that gives up
  // its rows when the terminal is short, and a list that just stops has no way
  // to say it was cut off.
  return Section("DETAILS", Scrollable(vbox(std::move(rows))));
}

// Skeletons rather than a blank panel: the shape of the answer is known before
// the answer is, and showing it stops the layout from jumping when data lands.
Element Waiting(const RemoteSnapshot& snapshot, int frame) {
  const Theme& t = theme();
  return Section("LOADING", vbox({
                                hbox({
                                    text("  "),
                                    text(SpinnerFrame(frame)) | bold | color(t.accent),
                                    text("  asking " + snapshot.ref.host + " about " +
                                         snapshot.ref.full_name()) |
                                        color(t.text_dim),
                                    filler(),
                                }),
                                text(""),
                                SkeletonRows(5, frame),
                                text(""),
                            }));
}

Element Problem(const RemoteSnapshot& snapshot) {
  const Theme& t = theme();
  Elements rows{
      hbox({
          text("  "),
          text(glyphs().cross) | bold | color(t.danger),
          text("  " + snapshot.error) | bold | color(t.text),
          filler(),
      }),
  };
  if (!snapshot.hint.empty()) {
    rows.push_back(text(""));
    rows.push_back(hbox({text("     "), text(snapshot.hint) | color(t.text_dim), filler()}));
  }
  rows.push_back(text(""));
  rows.push_back(hbox({text("  "), Chip("r", "try again"), filler()}));
  rows.push_back(text(""));

  return Panel("COULD NOT REACH THE REMOTE", vbox(std::move(rows)), {.alarm = true});
}

// Not an error. Plenty of repositories have no remote, or a remote gittop does
// not speak, and the local half of the dashboard is unaffected by that.
Element Unsupported(const RemoteSnapshot& snapshot) {
  const Theme& t = theme();
  const bool has_url = !snapshot.ref.url.empty();

  Elements rows{
      filler(),
      hbox({filler(), text(glyphs().empty_generic) | bold | color(t.text_dim), filler()}),
      text(""),
      hbox({filler(),
            text(has_url ? "no GitHub or GitLab remote" : "this repository has no remote") |
                color(t.text),
            filler()}),
      text(""),
  };

  if (has_url) {
    rows.push_back(hbox({filler(), text(snapshot.ref.name + "  " + SafeUrl(snapshot.ref.url)) |
                                       color(t.text_faint),
                         filler()}));
    rows.push_back(text(""));
    rows.push_back(hbox({filler(),
                         text("name its provider under hosts.\"" + snapshot.ref.host +
                              "\" in your config to read it") |
                             color(t.text_faint),
                         filler()}));
  } else {
    rows.push_back(hbox({filler(), text("the other four views work exactly as they do now") |
                                       color(t.text_faint),
                         filler()}));
  }

  rows.push_back(filler());
  return Section("REMOTE", vbox(std::move(rows)));
}

}  // namespace

Element RemotePanel(const RemoteSnapshot& snapshot, int width, int height, int frame,
                    const std::string& transports) {
  // Below this the tile row stops being four readable numbers and starts being
  // four clipped ones, so it folds into the details list instead.
  const bool wide = width >= 72;
  // And below this there are not enough rows for every panel to be worth its
  // border, so the least important one steps aside rather than every panel
  // being squeezed into something none of them can be read at.
  const bool tall = height >= 26;

  if (!snapshot.ref.valid()) {
    // flex on the window itself, not on a vbox wrapped around it: stretching
    // the container leaves the box its content size and the empty half below.
    return Unsupported(snapshot) | flex;
  }

  Elements body{Identity(snapshot)};

  switch (snapshot.state) {
    case FetchState::Idle:
    case FetchState::Loading:
      body.push_back(Waiting(snapshot, frame));
      break;

    case FetchState::Failed:
      body.push_back(Problem(snapshot));
      // What went wrong and what to do about it is the whole message here. The
      // reference data underneath is worth having, but not worth crowding it.
      if (tall) {
        body.push_back(Details(snapshot, false, transports));
      }
      break;

    case FetchState::Ready:
      if (wide && tall) {
        body.push_back(Tiles(snapshot));
      }
      body.push_back(Budget(snapshot));
      body.push_back(Details(snapshot, !wide || !tall, transports));
      break;
  }

  // The last panel takes the slack instead of a filler, so the view reaches the
  // bottom of the terminal. Boxes floating above an empty half look unfinished;
  // a frame that meets the footer looks like the screen was meant to be that
  // size, which is the whole trick btop pulls.
  body.back() = std::move(body.back()) | flex;
  return vbox(std::move(body));
}

}  // namespace gittop::ui

#include "ui/pull_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "ui/glyphs.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using model::FetchState;
using model::MergeStatus;
using model::Provider;
using model::PullRequest;
using model::PullSnapshot;
using model::PullState;
using model::RemoteRef;

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

std::int64_t Now() {
  return static_cast<std::int64_t>(std::time(nullptr));
}

std::string Ago(std::int64_t when) {
  if (when <= 0) {
    return "";
  }
  const std::int64_t delta = std::max<std::int64_t>(0, Now() - when);
  if (delta < 60) {
    return "now";
  }
  if (delta < 3600) {
    return std::to_string(delta / 60) + "m";
  }
  if (delta < 86400) {
    return std::to_string(delta / 3600) + "h";
  }
  if (delta < 86400LL * 30) {
    return std::to_string(delta / 86400) + "d";
  }
  if (delta < 86400LL * 365) {
    return std::to_string(delta / (86400LL * 30)) + "mo";
  }
  return std::to_string(delta / (86400LL * 365)) + "y";
}

// Draft is a modifier on open rather than a state of its own, but it is the
// thing a reviewer needs first — a draft is not asking for anything yet — so it
// takes the state column when it applies.
std::string StateGlyph(const PullRequest& pull) {
  const GlyphSet& g = glyphs();
  if (pull.draft && pull.state == PullState::Open) {
    return g.ci_pending;
  }
  switch (pull.state) {
    case PullState::Open:
      return g.unstaged;
    case PullState::Merged:
      return g.conflict;
    case PullState::Closed:
      return g.cross;
    case PullState::Unknown:
      break;
  }
  return "?";
}

std::string StateWord(const PullRequest& pull) {
  if (pull.draft && pull.state == PullState::Open) {
    return "draft";
  }
  return model::PullStateName(pull.state);
}

Swatch StateColor(const PullRequest& pull) {
  const Theme& t = theme();
  if (pull.draft && pull.state == PullState::Open) {
    return t.text_faint;
  }
  switch (pull.state) {
    case PullState::Open:
      return t.success;
    case PullState::Merged:
      return t.untracked;
    case PullState::Closed:
      return t.danger;
    case PullState::Unknown:
      break;
  }
  return t.text_faint;
}

Swatch MergeColor(MergeStatus status) {
  const Theme& t = theme();
  switch (status) {
    case MergeStatus::Clean:
      return t.success;
    case MergeStatus::Conflict:
      return t.danger;
    case MergeStatus::Blocked:
      return t.warning;
    case MergeStatus::Checking:
      return t.text_dim;
    case MergeStatus::Unknown:
      break;
  }
  return t.text_faint;
}

// What the provider calls them. Printing "pull requests" at a GitLab user is
// the sort of small wrongness that makes a tool feel like it was built for
// somewhere else.
std::string Noun(Provider provider, bool plural) {
  if (provider == Provider::GitLab) {
    return plural ? "merge requests" : "merge request";
  }
  return plural ? "pull requests" : "pull request";
}

std::string ShortNoun(Provider provider) {
  return provider == Provider::GitLab ? "MR" : "PR";
}

Element Section(const std::string& title, Element body, bool focused = false) {
  return Panel(title, std::move(body), {.focused = focused});
}

Element Tag(const std::string& label, Swatch tone) {
  return text(" " + label + " ") | color(theme().bg) | bgcolor(tone);
}

Element Header(const PullSnapshot& snapshot, const RemoteRef& ref, bool fetching, int frame) {
  const Theme& t = theme();

  Elements row{
      text(" " + ProviderGlyph(ref.provider) + " ") | bold | color(t.accent),
      text(ref.full_name()) | bold | color(t.text),
  };

  if (snapshot.state == FetchState::Ready) {
    const auto count = snapshot.pulls.size();
    row.push_back(text("  " + std::to_string(count) + " open " +
                       Noun(ref.provider, count != 1)) |
                  color(t.text_faint));
    if (snapshot.drafts > 0) {
      row.push_back(text("  " + std::to_string(snapshot.drafts) + " draft") |
                    color(t.text_faint));
    }
    if (snapshot.conflicted > 0) {
      row.push_back(text("  "));
      row.push_back(Tag(std::to_string(snapshot.conflicted) + " conflicting", t.danger));
    }
  }

  row.push_back(filler());
  if (fetching) {
    row.push_back(text(SpinnerFrame(frame)) | color(t.accent));
    row.push_back(text(" reading") | color(t.text_dim));
  } else if (snapshot.fetched_at > 0) {
    row.push_back(text("updated " + Ago(snapshot.fetched_at)) | color(t.text_faint));
  }
  row.push_back(text(" "));

  return hbox(std::move(row)) | borderRounded | color(t.border) | bgcolor(t.surface);
}

Element PullRow(const PullRequest& pull, bool selected, bool wide) {
  const Theme& t = theme();
  const Swatch tone = StateColor(pull);

  Elements row{
      text(selected ? glyphs().cursor : " ") | color(t.accent),
      text(" "),
      text(StateGlyph(pull)) | bold | color(tone),
      text(" "),
      text(Fit(StateWord(pull), 8)) | color(tone),
      text(Fit(pull.number >= 0 ? "#" + std::to_string(pull.number) : "", 7)) |
          color(t.text_faint),
  };

  // The one on the branch you are standing on is nearly always the reason the
  // view was opened, and it is already sorted to the top; this says why it is
  // there rather than leaving the order looking arbitrary.
  row.push_back(text(pull.from_head ? std::string(glyphs().pan_left) + " " : "  ") | bold |
                color(t.accent));
  row.push_back(text(pull.title) | color(selected ? t.text : t.text_dim) | xflex);

  if (wide) {
    row.push_back(text("  "));
    row.push_back(text(Fit(pull.source_branch, 20)) | color(t.text_faint));
    row.push_back(text(std::string(glyphs().arrow_right) + " ") | color(t.text_faint));
    row.push_back(text(Fit(pull.target_branch, 14)) | color(t.text_faint));
  }

  // Only GitLab reports mergeability on a list, so the column exists only when
  // something filled it rather than reading "unknown" down every GitHub row.
  if (pull.merge_status != MergeStatus::Unknown) {
    row.push_back(text(Fit(model::MergeStatusName(pull.merge_status), 11)) |
                  color(MergeColor(pull.merge_status)));
  }

  row.push_back(text("  "));
  row.push_back(text(Fit(pull.author, 14)) | color(t.text_faint));
  row.push_back(text(Rjust(Ago(pull.updated_at), 4)) | color(t.text_faint));
  row.push_back(text(" "));

  Element element = hbox(std::move(row));
  if (selected) {
    element = element | bgcolor(t.surface_alt) | focus;
  }
  return element;
}

Element EmptyState(const RemoteRef& ref) {
  const Theme& t = theme();
  return vbox({
      filler(),
      hbox({filler(), text(glyphs().empty_pull) | bold | color(t.text_dim), filler()}),
      text(""),
      hbox({filler(), text("no open " + Noun(ref.provider, true)) | color(t.text), filler()}),
      text(""),
      hbox({filler(),
            text("one appears here as soon as somebody opens it") | color(t.text_faint),
            filler()}),
      filler(),
  });
}

Element PullList(const PullSnapshot& snapshot, const RemoteRef& ref, const PullView& view,
                 bool wide, std::vector<Box>* boxes) {
  // ref is only here for the empty state's wording — "no open merge requests"
  // against "no open pull requests".
  if (snapshot.pulls.empty()) {
    return EmptyState(ref);
  }

  if (boxes != nullptr) {
    boxes->assign(snapshot.pulls.size(), Box());
  }

  Elements rows;
  rows.reserve(snapshot.pulls.size());
  for (std::size_t i = 0; i < snapshot.pulls.size(); ++i) {
    Element row = PullRow(snapshot.pulls[i], static_cast<int>(i) == view.selected, wide);
    if (boxes != nullptr) {
      row = std::move(row) | reflect((*boxes)[i]);
    }
    rows.push_back(std::move(row));
  }
  return Scrollable(vbox(std::move(rows)));
}

// Everything here arrived with the list, which is why `enter` opens it
// immediately and why moving the cursor does not have to close it: unlike the
// CI view's jobs, nothing about this pane costs a request.
Element DetailPane(const PullRequest& pull, const RemoteRef& ref) {
  const Theme& t = theme();
  const std::string title =
      ShortNoun(ref.provider) + (pull.number >= 0 ? " #" + std::to_string(pull.number) : "");

  const auto field = [&t](const std::string& label, Element value) {
    return hbox({
        text("  "),
        text(label) | color(t.text_faint) | size(WIDTH, EQUAL, 12),
        std::move(value),
        filler(),
    });
  };

  Elements rows{
      hbox({text("  "), text(pull.title) | bold | color(t.text), filler()}),
      text(""),
      field("branch", hbox({
                          text(pull.source_branch) | color(t.accent),
                          text("  " + std::string(glyphs().arrow_right) + "  ") |
                              color(t.text_faint),
                          text(pull.target_branch) | color(t.text_dim),
                      })),
      field("author", text(pull.author.empty() ? "unknown" : pull.author) | color(t.text_dim)),
      field("opened", text(Ago(pull.created_at).empty() ? "unknown" : Ago(pull.created_at) +
                                                                          " ago") |
                          color(t.text_dim)),
      field("updated", text(Ago(pull.updated_at).empty() ? "unknown" : Ago(pull.updated_at) +
                                                                           " ago") |
                           color(t.text_dim)),
  };

  if (pull.merge_status != MergeStatus::Unknown) {
    rows.push_back(field("merge", text(model::MergeStatusName(pull.merge_status)) |
                                      color(MergeColor(pull.merge_status))));
  }
  if (pull.comments >= 0) {
    rows.push_back(field("comments", text(std::to_string(pull.comments)) | color(t.text_dim)));
  }

  if (!pull.reviewers.empty()) {
    std::string names;
    for (const std::string& name : pull.reviewers) {
      names += names.empty() ? name : ", " + name;
    }
    rows.push_back(field("reviewers", text(names) | color(t.text_dim)));
  }

  if (!pull.labels.empty()) {
    Elements tags{text("  "), text("labels") | color(t.text_faint) | size(WIDTH, EQUAL, 12)};
    for (const std::string& label : pull.labels) {
      tags.push_back(text(" " + label + " ") | color(t.text_dim) | bgcolor(t.surface_raised));
      tags.push_back(text(" "));
    }
    tags.push_back(filler());
    rows.push_back(hbox(std::move(tags)));
  }

  if (!pull.web_url.empty()) {
    // Printed rather than opened. gittop does not launch a browser, and a URL
    // on screen is something a terminal can already copy.
    rows.push_back(text(""));
    rows.push_back(field("url", text(pull.web_url) | color(t.text_faint)));
  }

  return Section(title, vbox(std::move(rows)));
}

Element Waiting(const RemoteRef& ref, int frame) {
  const Theme& t = theme();
  return Section("LOADING", vbox({
                                hbox({
                                    text("  "),
                                    text(SpinnerFrame(frame)) | bold | color(t.accent),
                                    text("  asking " + ref.host + " for open " +
                                         Noun(ref.provider, true)) |
                                        color(t.text_dim),
                                    filler(),
                                }),
                                text(""),
                                SkeletonRows(5, frame),
                                text(""),
                            }));
}

Element Problem(const PullSnapshot& snapshot) {
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

  return Panel("COULD NOT READ PULL REQUESTS", vbox(std::move(rows)), {.alarm = true});
}

Element Unsupported(const RemoteRef& ref) {
  const Theme& t = theme();
  const bool has_url = !ref.url.empty();
  return Section("PULL REQUESTS",
                 vbox({
                     filler(),
                     hbox({filler(), text(glyphs().empty_generic) | bold | color(t.text_dim),
                           filler()}),
                     text(""),
                     hbox({filler(),
                           text(has_url ? "no GitHub or GitLab remote"
                                        : "this repository has no remote") |
                               color(t.text),
                           filler()}),
                     text(""),
                     hbox({filler(),
                           text("pull requests come from the remote; the local views do not") |
                               color(t.text_faint),
                           filler()}),
                     filler(),
                 }));
}

}  // namespace

Element PullPanel(const PullSnapshot& snapshot, const RemoteRef& ref, const PullView& view,
                  int width, int height, int frame, std::vector<Box>* rows) {
  if (rows != nullptr) {
    rows->clear();
  }
  if (!ref.valid()) {
    return Unsupported(ref) | flex;
  }

  // Below this the two branch names cannot both fit without pushing the author
  // off the row, and the detail pane spells them out anyway.
  const bool wide = width >= 100;
  const bool fetching = snapshot.state == FetchState::Loading;

  Elements body{Header(snapshot, ref, fetching, frame)};
  bool stretched = false;

  switch (snapshot.state) {
    case FetchState::Idle:
    case FetchState::Loading:
      body.push_back(Waiting(ref, frame));
      break;

    case FetchState::Failed:
      body.push_back(Problem(snapshot));
      break;

    case FetchState::Ready: {
      const bool have_selection =
          view.selected >= 0 && view.selected < static_cast<int>(snapshot.pulls.size());
      const bool detailing = view.details_open && have_selection;
      const bool details_take_the_slack = detailing && height < 26;

      Element list =
          Section(Noun(ref.provider, true) == "merge requests" ? "MERGE REQUESTS"
                                                               : "PULL REQUESTS",
                  PullList(snapshot, ref, view, wide, rows), /*focused=*/true);
      if (!details_take_the_slack) {
        list = std::move(list) | flex;
        stretched = true;
      }
      body.push_back(std::move(list));

      if (detailing) {
        Element pane = DetailPane(snapshot.pulls[static_cast<std::size_t>(view.selected)], ref);
        if (details_take_the_slack) {
          pane = std::move(pane) | flex;
          stretched = true;
        } else {
          pane = std::move(pane) | size(HEIGHT, LESS_THAN, std::max(8, height / 2));
        }
        body.push_back(std::move(pane));
      }
      break;
    }
  }

  if (!stretched) {
    body.back() = std::move(body.back()) | flex;
  }
  return vbox(std::move(body));
}

Element PullComposePane(const PullComposeView& view, Element source, Element target,
                        Element title, Element body) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();

  // One row per box: a label, the caret on the one with focus, and the input.
  // The caret rather than a highlighted label, because the label is what the
  // field *is* and the caret is where the typing goes — and on the terminals
  // that draw the cursor themselves, two markers would disagree.
  const auto row = [&t, &g, &view](int field, const std::string& label, Element input) {
    const bool focused = view.field == field && !view.sending;
    return hbox({
        text("  "),
        text(focused ? g.prompt : " ") | bold | color(t.accent),
        text(" "),
        text(Fit(label, 7)) | color(focused ? t.text : t.text_faint),
        std::move(input) | flex,
        text(" "),
    });
  };

  Elements rows{
      hbox({
          text(" " + ProviderGlyph(view.provider) + " ") | bold | color(t.accent),
          text("Open a " + Noun(view.provider, /*plural=*/false)) | bold | color(t.text),
          filler(),
          text(view.full_name + " ") | color(t.text_faint),
      }),
      separator() | color(t.border),
      row(kPullSource, "from", std::move(source)),
      row(kPullTarget, "into", std::move(target)),
      row(kPullTitle, "title", std::move(title)),
      row(kPullBody, "body", std::move(body)),
  };

  // The preconditions, in the order they matter. Each one names the key that
  // fixes it rather than describing what to go and do elsewhere.
  Elements notes;
  const auto note = [&notes](Element glyph, Element line) {
    notes.push_back(hbox({text("  "), std::move(glyph), text("  "), std::move(line), filler()}));
  };

  if (!view.branch_on_remote) {
    const std::string what =
        view.upstream_gone
            ? "its upstream " + view.upstream + " is gone from the remote"
            : "it is not on " + (view.remote_name.empty() ? "the remote" : view.remote_name)
                  + " yet";
    note(text(g.gone) | bold | color(t.danger),
         hbox({
             text("This branch cannot be merged from: ") | color(t.text),
             text(what) | color(t.text_dim),
         }));
    note(text(" "), hbox({
                        text("push it first") | color(t.text_dim),
                        text(view.push_key.empty() ? "" : "  ") ,
                        view.push_key.empty() ? text("") : Chip(view.push_key, "push"),
                    }));
  } else if (view.ahead > 0) {
    // Not a refusal: the branch is there and the create succeeds. It simply
    // will not contain these commits, which is the kind of thing found out on
    // the review page an hour later.
    note(text(g.ahead) | bold | color(t.warning),
         hbox({
             text(std::to_string(view.ahead) +
                  (view.ahead == 1 ? " commit is not pushed" : " commits are not pushed")) |
                 color(t.text),
             text(", so it will open without ") | color(t.text_dim),
             text(view.ahead == 1 ? "it" : "them") | color(t.text_dim),
         }));
  }

  if (!view.default_branch_known) {
    note(text(g.absent) | color(t.text_dim),
         text("the default branch is still loading; type a target, or wait") |
             color(t.text_dim));
  }

  if (!view.error.empty()) {
    note(text(g.cross) | bold | color(t.danger), text(view.error) | bold | color(t.text));
    if (!view.hint.empty()) {
      note(text(" "), text(view.hint) | color(t.text_dim));
    }
  }

  if (!notes.empty()) {
    rows.push_back(separator() | color(t.border));
    for (Element& line : notes) {
      rows.push_back(std::move(line));
    }
  }

  rows.push_back(separator() | color(t.border));

  if (view.sending) {
    rows.push_back(hbox({
        text("  "),
        text("opening it on " + view.full_name) | color(t.text_dim),
        filler(),
        Chip("esc", "stop waiting"),
    }));
  } else {
    rows.push_back(hbox({
        text(" "),
        Chip("enter", view.branch_on_remote ? "open" : "blocked"),
        text("  "),
        Chip("tab", "next field"),
        filler(),
        Chip("esc", "cancel"),
    }));
  }

  return vbox(std::move(rows)) | PaneFrame() | size(WIDTH, GREATER_THAN, 64);
}

}  // namespace gittop::ui

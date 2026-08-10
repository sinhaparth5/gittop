#include "ui/pipeline_panel.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using model::FetchState;
using model::Job;
using model::JobList;
using model::Pipeline;
using model::PipelineSnapshot;
using model::Provider;
using model::RemoteRef;
using model::RunStatus;

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

std::int64_t Now() {
  return static_cast<std::int64_t>(std::time(nullptr));
}

// Short form, because this is a dense list rather than a details block. The
// remote view spells the same idea out in words; here a column four cells wide
// has to carry it.
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

std::string Pad2(std::int64_t value) {
  const std::string digits = std::to_string(value);
  return digits.size() < 2 ? "0" + digits : digits;
}

// Durations read as minutes and seconds up to an hour, which is where nearly
// every CI run lives. An em dash rather than "0s" for the ones still going:
// zero is a measurement and this is the absence of one.
std::string Duration(int seconds) {
  if (seconds < 0) {
    return "—";
  }
  if (seconds < 60) {
    return std::to_string(seconds) + "s";
  }
  if (seconds < 3600) {
    return std::to_string(seconds / 60) + "m " + Pad2(seconds % 60) + "s";
  }
  return std::to_string(seconds / 3600) + "h " + Pad2((seconds % 3600) / 60) + "m";
}

// How long a run has been going, for the ones that have not finished. Without
// this a running job shows an em dash for its whole life, which is exactly when
// the number is most worth having.
std::string Elapsed(const Pipeline& run) {
  if (run.duration_seconds >= 0) {
    return Duration(run.duration_seconds);
  }
  if (run.status == RunStatus::Running && run.started_at > 0) {
    return Duration(static_cast<int>(std::max<std::int64_t>(0, Now() - run.started_at)));
  }
  return "—";
}

// Glyph *and* word, never colour alone: a red cross and a green tick have to
// stay distinguishable to someone who cannot separate the two hues, and on a
// list this dense the word is the thing that does it.
std::string StatusGlyph(RunStatus status, int frame) {
  switch (status) {
    case RunStatus::Success:
      return "✓";
    case RunStatus::Failed:
      return "✗";
    case RunStatus::Running:
      return SpinnerFrame(frame);
    case RunStatus::Queued:
      return "◌";
    case RunStatus::Manual:
      return "▷";
    case RunStatus::Cancelled:
      return "⊘";
    case RunStatus::Skipped:
      return "–";
    case RunStatus::Unknown:
      break;
  }
  return "?";
}

Swatch StatusColor(RunStatus status) {
  const Theme& t = theme();
  switch (status) {
    case RunStatus::Success:
      return t.success;
    case RunStatus::Failed:
      return t.danger;
    case RunStatus::Running:
      return t.accent;
    case RunStatus::Queued:
      return t.unstaged;
    case RunStatus::Manual:
      return t.warning;
    case RunStatus::Cancelled:
    case RunStatus::Skipped:
    case RunStatus::Unknown:
      break;
  }
  return t.text_faint;
}

std::string ProviderGlyph(Provider provider) {
  switch (provider) {
    case Provider::GitHub:
      return "⬢";
    case Provider::GitLab:
      return "⬡";
    case Provider::Unknown:
      break;
  }
  return "○";
}

std::string CiName(Provider provider) {
  switch (provider) {
    case Provider::GitHub:
      return "GitHub Actions";
    case Provider::GitLab:
      return "GitLab CI";
    case Provider::Unknown:
      break;
  }
  return "CI";
}

Element Section(const std::string& title, Element body) {
  const Theme& t = theme();
  return window(text(" " + title + " ") | bold | color(t.text_dim), std::move(body)) |
         color(t.border) | bgcolor(t.surface);
}

// Where the refresh loop is up to. A dashboard that has quietly stopped
// updating looks exactly like one where nothing is happening, so this line says
// which of the two it is rather than leaving it to be inferred.
Element RefreshNote(const PipelineView& view, bool fetching, int frame) {
  const Theme& t = theme();
  if (fetching) {
    return hbox({
        text(SpinnerFrame(frame)) | color(t.accent),
        text(" refreshing") | color(t.text_dim),
    });
  }
  if (view.auto_paused) {
    return hbox({
        text("‖ ") | color(t.warning),
        text(view.paused_reason.empty() ? "auto-refresh paused" : view.paused_reason) |
            color(t.text_faint),
    });
  }
  if (view.next_refresh >= 0) {
    return text("next refresh in " + std::to_string(view.next_refresh) + "s") |
           color(t.text_faint);
  }
  return text("manual refresh") | color(t.text_faint);
}

Element Header(const PipelineSnapshot& snapshot, const RemoteRef& ref, const PipelineView& view,
               bool fetching, int frame) {
  const Theme& t = theme();

  Elements row{
      text(" " + ProviderGlyph(ref.provider) + " ") | bold | color(t.accent),
      text(CiName(ref.provider)) | bold | color(t.text),
  };

  if (snapshot.branch.empty()) {
    // A detached HEAD has no branch to filter by. Saying so beats an unlabelled
    // list that quietly spans every branch in the repository.
    row.push_back(text("  all branches ") | color(t.bg) | bgcolor(t.text_faint));
  } else {
    row.push_back(text("  " + snapshot.branch + " ") | bold | color(t.bg) | bgcolor(t.accent));
  }

  if (snapshot.state == FetchState::Ready) {
    const auto count = snapshot.runs.size();
    row.push_back(text("  " + std::to_string(count) + (count == 1 ? " run" : " runs")) |
                  color(t.text_faint));
  }

  row.push_back(filler());
  row.push_back(RefreshNote(view, fetching, frame));
  row.push_back(text(" "));

  return hbox(std::move(row)) | borderRounded | color(t.border) | bgcolor(t.surface);
}

// One line rather than four bordered tiles: the counts here are a tally, not
// four independent readings, and the runs list is what the rows are for.
Element Counts(const PipelineSnapshot& snapshot) {
  const Theme& t = theme();

  const auto entry = [&t](RunStatus status, int count, const std::string& label) {
    return hbox({
        text(StatusGlyph(status, 0)) | bold | color(count > 0 ? StatusColor(status) : t.text_faint),
        text(" " + std::to_string(count) + " " + label) |
            color(count > 0 ? t.text : t.text_faint),
        text("   "),
    });
  };

  return hbox({
             text("  "),
             entry(RunStatus::Success, snapshot.succeeded, "passed"),
             entry(RunStatus::Failed, snapshot.failed, "failed"),
             entry(RunStatus::Running, snapshot.running, "running"),
             entry(RunStatus::Queued, snapshot.pending, "waiting"),
             filler(),
             text(snapshot.fetched_at > 0 ? "updated " + Ago(snapshot.fetched_at) + "  " : "") |
                 color(t.text_faint),
         }) |
         bgcolor(t.surface);
}

Element RunRow(const Pipeline& run, bool selected, bool wide, int frame) {
  const Theme& t = theme();
  const Swatch status_color = StatusColor(run.status);

  // The workflow name on GitHub, the source on GitLab, which is the closest
  // thing it has to one. Falls back to the event so the column is never blank.
  std::string title = run.title.empty() ? run.event : run.title;

  Elements row{
      text(selected ? "▌" : " ") | color(t.accent),
      text(" "),
      text(StatusGlyph(run.status, frame)) | bold | color(status_color),
      text(" "),
      // Ten, not nine: "cancelled" is exactly nine and would touch the run
      // number with no gap at all.
      text(model::RunStatusName(run.status)) | color(status_color) | size(WIDTH, EQUAL, 10),
      text(run.number >= 0 ? "#" + std::to_string(run.number) : "") | color(t.text_faint) |
          size(WIDTH, EQUAL, 7),
      text(title) | color(selected ? t.text : t.text_dim) | size(WIDTH, EQUAL, 18),
      text("  "),
      text(run.short_sha()) | color(t.accent) | size(WIDTH, EQUAL, 8),
  };

  if (wide) {
    // The commit title is the first thing to go when the terminal narrows: it
    // is the widest column and the only one whose absence loses nothing that
    // cannot be read off the row above it in the log view.
    row.push_back(text(run.commit_title) | color(t.text_dim) | xflex);
    if (!run.actor.empty()) {
      row.push_back(text("  " + run.actor) | color(t.text_faint));
    }
  } else {
    row.push_back(filler());
  }

  row.push_back(text("  "));
  row.push_back(text(Elapsed(run)) | color(t.text_faint) | size(WIDTH, EQUAL, 8));
  row.push_back(text(Ago(run.created_at)) | color(t.text_faint) | size(WIDTH, EQUAL, 4));
  row.push_back(text(" "));

  Element element = hbox(std::move(row));
  if (selected) {
    element = element | bgcolor(t.surface_alt) | focus;
  }
  return element;
}

Element RunList(const PipelineSnapshot& snapshot, const PipelineView& view, bool wide, int frame) {
  const Theme& t = theme();

  if (snapshot.runs.empty()) {
    const std::string where =
        snapshot.branch.empty() ? "this repository" : "branch " + snapshot.branch;
    return vbox({
        filler(),
        hbox({filler(), text("◌") | bold | color(t.text_dim), filler()}),
        text(""),
        hbox({filler(), text("no CI runs for " + where) | color(t.text), filler()}),
        text(""),
        hbox({filler(),
              text("a run appears here the moment one starts") | color(t.text_faint),
              filler()}),
        filler(),
    });
  }

  Elements rows;
  rows.reserve(snapshot.runs.size());
  for (std::size_t i = 0; i < snapshot.runs.size(); ++i) {
    rows.push_back(
        RunRow(snapshot.runs[i], static_cast<int>(i) == view.selected, wide, frame));
  }
  return vbox(std::move(rows)) | vscroll_indicator | yframe;
}

Element JobRow(const Job& job, bool show_stage, int frame) {
  const Theme& t = theme();
  const Swatch status_color = StatusColor(job.status);

  Elements row{
      text("  "),
      text(StatusGlyph(job.status, frame)) | bold | color(status_color),
      text(" "),
      text(model::RunStatusName(job.status)) | color(status_color) | size(WIDTH, EQUAL, 10),
  };
  if (show_stage) {
    row.push_back(text(job.stage) | color(t.text_faint) | size(WIDTH, EQUAL, 12));
  }
  row.push_back(text(job.name) | color(t.text) | xflex);
  row.push_back(text("  "));
  row.push_back(text(Duration(job.duration_seconds)) | color(t.text_faint) |
                size(WIDTH, EQUAL, 8));
  row.push_back(text(" "));
  return hbox(std::move(row));
}

Element JobsPane(const JobList& jobs, const Pipeline& run, int frame) {
  const Theme& t = theme();
  const std::string title =
      "JOBS · " + (run.number >= 0 ? "#" + std::to_string(run.number) : run.short_sha());

  // The jobs on screen have to belong to the run under the cursor. Moving the
  // selection while a fetch is in flight would otherwise label the previous
  // run's jobs with the new run's number.
  const bool stale = jobs.pipeline_id != run.id;

  // A refresh of the same run keeps its rows on screen. Dropping back to the
  // spinner every twenty seconds would make a pane that is working look like
  // one that keeps starting over.
  const bool nothing_yet = jobs.jobs.empty();
  const bool pending = jobs.state == FetchState::Loading || jobs.state == FetchState::Idle;

  if (stale || (pending && nothing_yet)) {
    return Section(title, hbox({
                              text("  "),
                              text(SpinnerFrame(frame)) | color(t.accent),
                              text("  reading jobs") | color(t.text_dim),
                              filler(),
                          }));
  }

  if (jobs.state == FetchState::Failed) {
    Elements rows{hbox({
        text("  "),
        text("✗") | bold | color(t.danger),
        text("  " + jobs.error) | color(t.text),
        filler(),
    })};
    if (!jobs.hint.empty()) {
      rows.push_back(hbox({text("     "), text(jobs.hint) | color(t.text_faint), filler()}));
    }
    return Section(title, vbox(std::move(rows)));
  }

  if (jobs.jobs.empty()) {
    return Section(title, hbox({
                              text("  "),
                              text("this run has no jobs") | color(t.text_faint),
                              filler(),
                          }));
  }

  // GitLab groups jobs into stages and GitHub does not, so the column appears
  // only when there is something to put in it.
  const bool show_stage = std::any_of(jobs.jobs.begin(), jobs.jobs.end(),
                                      [](const Job& job) { return !job.stage.empty(); });

  Elements rows;
  rows.reserve(jobs.jobs.size());
  for (const Job& job : jobs.jobs) {
    rows.push_back(JobRow(job, show_stage, frame));
  }
  return Section(title, vbox(std::move(rows)) | vscroll_indicator | yframe);
}

Element Waiting(const RemoteRef& ref, const PipelineSnapshot& snapshot, int frame) {
  const Theme& t = theme();
  const std::string where = snapshot.branch.empty() ? ref.full_name() : snapshot.branch;
  return Section("LOADING", vbox({
                                hbox({
                                    text("  "),
                                    text(SpinnerFrame(frame)) | bold | color(t.accent),
                                    text("  asking " + ref.host + " about " + where) |
                                        color(t.text_dim),
                                    filler(),
                                }),
                                text(""),
                                hbox({text("  "),
                                      text("            ") | bgcolor(t.surface_alt), filler()}),
                                text(""),
                                hbox({text("  "),
                                      text("                              ") |
                                          bgcolor(t.surface_alt),
                                      filler()}),
                                text(""),
                            }));
}

Element Problem(const PipelineSnapshot& snapshot) {
  const Theme& t = theme();
  Elements rows{
      hbox({
          text("  "),
          text("✗") | bold | color(t.danger),
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

  return window(text(" COULD NOT READ CI ") | bold | color(t.danger), vbox(std::move(rows))) |
         color(t.danger) | bgcolor(t.surface);
}

// Not an error, exactly as on the remote view: plenty of repositories have no
// remote gittop speaks, and every local view is unaffected by that.
Element Unsupported(const RemoteRef& ref) {
  const Theme& t = theme();
  const bool has_url = !ref.url.empty();
  return Section("PIPELINES", vbox({
                                  filler(),
                                  hbox({filler(), text("◇") | bold | color(t.text_dim), filler()}),
                                  text(""),
                                  hbox({filler(),
                                        text(has_url ? "no GitHub or GitLab remote"
                                                     : "this repository has no remote") |
                                            color(t.text),
                                        filler()}),
                                  text(""),
                                  hbox({filler(),
                                        text("CI comes from the remote; the local views do not") |
                                            color(t.text_faint),
                                        filler()}),
                                  filler(),
                              }));
}

}  // namespace

Element PipelinePanel(const PipelineSnapshot& snapshot, const JobList& jobs, const RemoteRef& ref,
                      const PipelineView& view, int width, int height, int frame) {
  if (!ref.valid()) {
    // flex on the window itself, not on a vbox wrapped around it.
    return Unsupported(ref) | flex;
  }

  // Below this the commit title has nowhere to go without pushing the duration
  // off the row, so it is the column that yields.
  const bool wide = width >= 96;
  const bool fetching = snapshot.state == FetchState::Loading;

  Elements body{Header(snapshot, ref, view, fetching, frame)};
  // Tracked rather than asked for afterwards: a node's requirement is only
  // filled in during layout, so there is no way to look at a built element and
  // find out whether it already stretches.
  bool stretched = false;

  switch (snapshot.state) {
    case FetchState::Idle:
    case FetchState::Loading:
      body.push_back(Waiting(ref, snapshot, frame));
      break;

    case FetchState::Failed:
      body.push_back(Problem(snapshot));
      break;

    case FetchState::Ready: {
      body.push_back(Counts(snapshot));

      const bool have_selection =
          view.selected >= 0 && view.selected < static_cast<int>(snapshot.runs.size());
      const bool drilling = view.jobs_open && have_selection;

      // The drill-down costs rows the run list would otherwise have, so on a
      // short terminal it takes the whole panel and the list keeps its content
      // size, rather than leaving two lists each too small to read.
      const bool jobs_take_the_slack = drilling && height < 24;

      Element runs = Section("RUNS", RunList(snapshot, view, wide, frame));
      if (!jobs_take_the_slack) {
        runs = std::move(runs) | flex;
        stretched = true;
      }
      body.push_back(std::move(runs));

      if (drilling) {
        const Pipeline& run = snapshot.runs[static_cast<std::size_t>(view.selected)];
        Element pane = JobsPane(jobs, run, frame);
        if (jobs_take_the_slack) {
          pane = std::move(pane) | flex;
          stretched = true;
        } else {
          pane = std::move(pane) | size(HEIGHT, LESS_THAN, std::max(6, height / 2));
        }
        body.push_back(std::move(pane));
      }
      break;
    }
  }

  // The last panel takes the slack so the view reaches the bottom of the
  // terminal rather than floating above an empty half.
  if (!stretched) {
    body.back() = std::move(body.back()) | flex;
  }
  return vbox(std::move(body));
}

}  // namespace gittop::ui

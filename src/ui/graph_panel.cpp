#include "ui/graph_panel.hpp"

#include <ftxui/dom/canvas.hpp>

#include <algorithm>
#include <array>
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

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

constexpr std::int64_t kSecondsPerDay = 86400;
constexpr int kAxisWidth = 6;

std::tm ToTm(std::int64_t epoch_day) {
  const auto seconds = static_cast<std::time_t>(epoch_day * kSecondsPerDay);
  std::tm out{};
  gmtime_r(&seconds, &out);
  return out;
}

// Epoch day 0 was a Thursday, so shifting by three puts Monday at zero.
std::int64_t WeekStart(std::int64_t day) {
  const std::int64_t weekday = (((day + 3) % 7) + 7) % 7;
  return day - weekday;
}

std::int64_t BucketKey(std::int64_t day, Bucket bucket) {
  switch (bucket) {
    case Bucket::Week:
      return WeekStart(day);
    case Bucket::Month: {
      const std::tm parts = ToTm(day);
      return (static_cast<std::int64_t>(parts.tm_year) * 12) + parts.tm_mon;
    }
    case Bucket::Day:
      break;
  }
  return day;
}

// Axis ticks stay short. Week and month windows cover a year or more, so those
// carry the year instead of the day: without it, two ticks twelve months apart
// both read "Aug 11" and the axis looks like it runs backwards.
std::string DayLabel(std::int64_t day, Bucket bucket) {
  const std::tm parts = ToTm(day);
  std::array<char, 16> buffer{};
  std::strftime(buffer.data(), buffer.size(), bucket == Bucket::Day ? "%b %d" : "%b %y",
                &parts);
  return buffer.data();
}

// The range line always spells out the year, whatever the bucket, because it is
// the one place the reader checks to find out exactly what is on screen.
std::string RangeLabel(std::int64_t day) {
  const std::tm parts = ToTm(day);
  std::array<char, 24> buffer{};
  std::strftime(buffer.data(), buffer.size(), "%b %d %Y", &parts);
  return buffer.data();
}

struct Series {
  std::vector<int> values;
  std::vector<std::int64_t> start_day;
};

Series Bucketize(const model::HistorySnapshot& history, Bucket bucket) {
  Series series;
  if (history.daily.empty()) {
    return series;
  }

  // The daily run is contiguous and ascending, so bucket keys arrive in order
  // and a new bucket starts exactly when the key changes.
  bool started = false;
  std::int64_t previous_key = 0;

  for (std::size_t i = 0; i < history.daily.size(); ++i) {
    const std::int64_t day = history.daily_start_day + static_cast<std::int64_t>(i);
    const std::int64_t key = BucketKey(day, bucket);

    if (!started || key != previous_key) {
      series.values.push_back(0);
      series.start_day.push_back(day);
      previous_key = key;
      started = true;
    }
    series.values.back() += history.daily[i];
  }
  return series;
}

Element BarRow(const std::string& label, int value, int peak, int label_width, Ramp ramp) {
  const Theme& t = theme();
  const float ratio = peak > 0 ? static_cast<float>(value) / static_cast<float>(peak) : 0.0F;
  return hbox({
      text(" " + label) | color(t.text_dim) | size(WIDTH, EQUAL, label_width),
      GradientBar(ratio, ramp, t.surface_alt),
      text(" " + std::to_string(value) + " ") | color(t.text_faint),
  });
}

// One character per hour so each tick sits under the bar it labels.
std::string HourAxis() {
  std::string axis(24, ' ');
  for (int hour = 0; hour < 24; hour += 3) {
    const std::string tick = std::to_string(hour);
    if (hour + static_cast<int>(tick.size()) <= 24) {
      axis.replace(static_cast<std::size_t>(hour), tick.size(), tick);
    }
  }
  return axis;
}

Element EmptyChart(const std::string& message) {
  const Theme& t = theme();
  return vbox({
      filler(),
      hbox({filler(), text(message) | color(t.text_faint), filler()}),
      filler(),
  });
}

}  // namespace

int GraphWindow(Bucket bucket) {
  switch (bucket) {
    case Bucket::Week:
      return 52;  // a year
    case Bucket::Month:
      return 24;  // two years
    case Bucket::Day:
      break;
  }
  return 90;  // a quarter
}

std::string BucketName(Bucket bucket) {
  switch (bucket) {
    case Bucket::Week:
      return "week";
    case Bucket::Month:
      return "month";
    case Bucket::Day:
      break;
  }
  return "day";
}

int GraphSeriesSize(const model::HistorySnapshot& history, Bucket bucket) {
  return static_cast<int>(Bucketize(history, bucket).values.size());
}

Element GraphPanel(const model::HistorySnapshot& history, const GraphView& view, int width,
                   int rows) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();
  const Series series = Bucketize(history, view.bucket);

  if (series.values.empty()) {
    return Panel("COMMIT ACTIVITY", EmptyChart("no commits to plot"), {.focused = false});
  }

  const auto total = static_cast<int>(series.values.size());
  const int window_size = std::min(GraphWindow(view.bucket), total);
  const int max_offset = std::max(0, total - window_size);
  const int offset = std::clamp(view.offset, 0, max_offset);
  const int first = total - window_size - offset;

  // Peak is taken from what is visible, not from all time, so panning into a
  // quiet stretch still shows its shape instead of a flat line on the floor.
  int peak = 1;
  for (int i = first; i < first + window_size; ++i) {
    peak = std::max(peak, series.values[static_cast<std::size_t>(i)]);
  }

  const int chart_cells = std::max(8, width - kAxisWidth - 4);
  const int chart_rows = std::max(4, rows);

  // Copied, not captured by reference: the canvas callback runs during Render,
  // long after this function has returned.
  const std::vector<int> visible(series.values.begin() + first,
                                 series.values.begin() + first + window_size);
  const Ramp ramp = t.staged_ramp;

  Element plot = canvas(chart_cells * 2, chart_rows * 4, [visible, peak, ramp](Canvas& c) {
    const int pixel_width = c.width();
    const int pixel_height = c.height();
    if (pixel_width <= 0 || pixel_height <= 1 || visible.empty()) {
      return;
    }

    const auto last = static_cast<float>(visible.size() - 1);

    for (int x = 0; x < pixel_width; ++x) {
      // Interpolate between neighbouring buckets rather than snapping each
      // column to one of them. Without this the plot is a row of detached
      // rectangles; with it, it is a line with a filled area under it.
      const float position =
          pixel_width > 1 ? (static_cast<float>(x) * last) / static_cast<float>(pixel_width - 1)
                          : 0.0F;
      const auto low = static_cast<std::size_t>(position);
      const std::size_t high = std::min(low + 1, visible.size() - 1);
      const float fraction = position - static_cast<float>(low);
      const float value = (static_cast<float>(visible[low]) * (1.0F - fraction)) +
                          (static_cast<float>(visible[high]) * fraction);

      const int filled =
          static_cast<int>((value * static_cast<float>(pixel_height - 1)) /
                           static_cast<float>(peak));
      const int top = std::clamp(pixel_height - 1 - filled, 0, pixel_height - 1);

      for (int y = pixel_height - 1; y >= top; --y) {
        const float height_ratio = static_cast<float>(pixel_height - 1 - y) /
                                   static_cast<float>(pixel_height - 1);
        c.DrawPoint(x, y, true, ToColor(Mix(ramp.from, ramp.to, height_ratio)));
      }
      // Brightest at the crest so the outline of the curve stays legible even
      // where the fill below it is dim.
      if (filled > 0) {
        c.DrawPoint(x, top, true, ToColor(ramp.to));
      }
    }
  });

  // Y axis, one row per canvas cell row.
  Elements axis_rows;
  for (int row = 0; row < chart_rows; ++row) {
    std::string label;
    if (row == 0) {
      label = std::to_string(peak);
    } else if (row == chart_rows - 1) {
      label = "0";
    } else if (row == chart_rows / 2 && peak >= 4) {
      // Below four the midpoint rounds to something already labelled, and two
      // identical numbers on one axis read as a bug.
      label = std::to_string(peak / 2);
    }
    axis_rows.push_back(hbox({
        filler(),
        text(label) | color(t.text_faint),
        text(std::string(" ") + (row == chart_rows - 1 ? g.axis_origin : g.axis_tick)) |
            color(t.border),
    }));
  }

  // X axis labels, painted into a fixed-width line so they land under the data
  // they describe rather than being spaced by a layout engine.
  std::string x_axis(static_cast<std::size_t>(chart_cells), ' ');
  const int label_count = std::clamp(chart_cells / 14, 2, 6);
  for (int k = 0; k < label_count; ++k) {
    const int bucket_index = (k * (window_size - 1)) / std::max(1, label_count - 1);
    const std::string label =
        DayLabel(series.start_day[static_cast<std::size_t>(first + bucket_index)], view.bucket);
    int position = (bucket_index * chart_cells) / std::max(1, window_size);
    position = std::clamp(position, 0, chart_cells - static_cast<int>(label.size()));
    x_axis.replace(static_cast<std::size_t>(position), label.size(), label);
  }

  // Scroll position: where the visible window sits inside the whole timeline.
  const int handle_start = (first * chart_cells) / std::max(1, total);
  const int handle_length = std::max(1, (window_size * chart_cells) / std::max(1, total));
  Elements track;
  for (int cell = 0; cell < chart_cells; ++cell) {
    const bool on_handle = cell >= handle_start && cell < handle_start + handle_length;
    track.push_back(text(on_handle ? g.track_handle : g.track) |
                    color(on_handle ? t.accent : t.surface_alt));
  }

  const std::string range =
      RangeLabel(series.start_day[static_cast<std::size_t>(first)]) + "  to  " +
      RangeLabel(series.start_day[static_cast<std::size_t>(first + window_size - 1)]);

  Element body = vbox({
      hbox({
          vbox(std::move(axis_rows)) | size(WIDTH, EQUAL, kAxisWidth),
          std::move(plot),
      }),
      hbox({
          text(std::string(kAxisWidth, ' ')),
          text(x_axis) | color(t.text_faint),
      }),
      text(""),
      hbox({
          text(std::string(" ") + g.pan_left + " ") |
              color(offset < max_offset ? t.accent : t.surface_alt),
          hbox(std::move(track)),
          text(std::string(" ") + g.pan_right + " ") |
              color(offset > 0 ? t.accent : t.surface_alt),
          filler(),
      }),
      hbox({
          text("   " + range) | color(t.text_dim),
          filler(),
          text("bucket: ") | color(t.text_faint),
          text(BucketName(view.bucket) + "  ") | bold | color(t.accent),
      }),
  });

  return Panel("COMMIT ACTIVITY", std::move(body), {.note = BucketName(view.bucket) + " buckets"});
}

Element InsightsRow(const model::HistorySnapshot& history, int width) {
  const Theme& t = theme();

  // ---------------------------------------------------------- top authors
  Elements author_rows;
  int author_peak = 1;
  for (const auto& [name, count] : history.authors) {
    author_peak = std::max(author_peak, count);
  }
  const std::size_t shown = std::min<std::size_t>(history.authors.size(), 6);
  for (std::size_t i = 0; i < shown; ++i) {
    // Truncate() rather than substr(): an author name is arbitrary user text and
    // cutting it at byte 13 lands mid-glyph on any name that is not ASCII.
    const std::string name = Truncate(history.authors[i].first, 14);
    author_rows.push_back(
        BarRow(name, history.authors[i].second, author_peak, 16, t.untracked_ramp));
  }
  if (author_rows.empty()) {
    author_rows.push_back(text("  no authors") | color(t.text_faint));
  }
  Element authors =
      Panel("TOP AUTHORS", vbox(std::move(author_rows)), {.focused = false}) | xflex;

  // -------------------------------------------------------------- weekday
  static constexpr std::array<const char*, 7> kDayNames = {"Sun", "Mon", "Tue", "Wed",
                                                           "Thu", "Fri", "Sat"};
  int weekday_peak = 1;
  for (const int count : history.weekday) {
    weekday_peak = std::max(weekday_peak, count);
  }
  Elements weekday_rows;
  // Monday first: how a working week is read, even though the array is Sunday
  // indexed because that is what the epoch-day arithmetic produces.
  for (int i = 1; i <= 7; ++i) {
    const std::size_t index = static_cast<std::size_t>(i % 7);
    weekday_rows.push_back(BarRow(kDayNames[index], history.weekday[index], weekday_peak, 5,
                                  t.staged_ramp));
  }
  Element weekdays =
      Panel("BY WEEKDAY", vbox(std::move(weekday_rows)), {.focused = false}) | xflex;

  // ----------------------------------------------------------------- hour
  int hour_peak = 1;
  for (const int count : history.hour) {
    hour_peak = std::max(hour_peak, count);
  }
  Element hours = Panel("BY HOUR",
                        vbox({
                            text(""),
                            hbox({text(" "), Sparkline(history.hour.data(), history.hour.size(),
                                                       hour_peak, t.unstaged_ramp)}),
                            hbox({text(" "), text(HourAxis()) | color(t.text_faint)}),
                            text(""),
                            hbox({filler(),
                                  text("commits by local hour ") | color(t.text_faint)}),
                        }),
                        {.focused = false});

  // Panels drop out as the terminal narrows rather than each being squeezed
  // until none of them are readable.
  if (width < 80) {
    return authors;
  }
  if (width < 118) {
    return hbox({std::move(authors), std::move(weekdays)});
  }
  return hbox({std::move(authors), std::move(weekdays), std::move(hours)});
}

}  // namespace gittop::ui

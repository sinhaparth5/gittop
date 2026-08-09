#pragma once

#include <ftxui/dom/elements.hpp>

#include "model/history.hpp"

namespace gittop::ui {

enum class Bucket { Day, Week, Month };

struct GraphView {
  Bucket bucket = Bucket::Day;
  int offset = 0;  // buckets scrolled back from the newest
};

// How many buckets are on screen at once, per granularity.
int GraphWindow(Bucket bucket);

// Total buckets available, so the caller can clamp its scroll offset.
int GraphSeriesSize(const model::HistorySnapshot& history, Bucket bucket);

std::string BucketName(Bucket bucket);

// `width` and `rows` are terminal cells. The canvas has to be sized explicitly
// because FTXUI's canvas(fn) overload looks like it auto-fits but actually
// hardcodes 12x12.
ftxui::Element GraphPanel(const model::HistorySnapshot& history, const GraphView& view,
                          int width, int rows);

ftxui::Element InsightsRow(const model::HistorySnapshot& history, int width);

}  // namespace gittop::ui

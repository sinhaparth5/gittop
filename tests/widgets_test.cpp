#include <doctest/doctest.h>

#include <string>

#include "ui/widgets.hpp"

using gittop::ui::Fit;
using gittop::ui::Rjust;
using gittop::ui::TextWidth;
using gittop::ui::Truncate;

TEST_CASE("width is measured in cells, not bytes") {
  CHECK(TextWidth("abc") == 3);
  CHECK(TextWidth("日本語") == 6);
  CHECK(TextWidth("") == 0);
}

TEST_CASE("truncation never tears a double-width glyph") {
  const std::string mixed = "a日本語b";
  for (int cells = 0; cells <= TextWidth(mixed) + 2; ++cells) {
    CAPTURE(cells);
    CHECK(TextWidth(Truncate(mixed, cells)) <= cells);
    // Fit and Rjust pad to the exact width whatever had to go, which is what
    // keeps the next column, and the right-hand border, where they were.
    CHECK(TextWidth(Fit(mixed, cells)) == cells);
    CHECK(TextWidth(Rjust(mixed, cells)) == cells);
  }
  CHECK(Truncate(mixed, TextWidth(mixed)) == mixed);
}

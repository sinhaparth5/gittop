#pragma once

#include <cstddef>
#include <ftxui/dom/elements.hpp>
#include <string>

#include "model/conflict.hpp"

namespace gittop::ui {

// What the resolver overlay needs that the file itself does not say: where it
// sits among the conflicted files, what stopped, and which editor `e` opens.
struct ConflictView {
  const model::ConflictFile* file = nullptr;
  int hunk = 0;
  std::size_t file_index = 0;  // 0-based
  std::size_t file_count = 0;
  std::string operation;  // "merge", "rebase", ...
  std::string editor;     // argv[0] of the editor, for the chip
};

// Ours and theirs side by side for one block at a time, with the lines around
// it, and the keys that pick a side. A file with no blocks to show (binary, a
// side that deleted it, or one already edited clean) says which of the whole-
// file answers applies instead.
ftxui::Element ConflictPane(const ConflictView& view, int width, int height);

}  // namespace gittop::ui
